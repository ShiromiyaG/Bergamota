// A port's launcher (Solstice, Duality, Tidefall: which, and its games, its
// port.toml says). It opens on the port's games: one set up plays, one not
// yet asks for the player's own dump and its update, checks them, and has
// 3dsrecomp recompile the game on this computer, to run on the runtime's
// player; nothing made from the game ships with the port. `--play [title]`
// starts the game played last (or that one) straight away.
//
// The first build only takes the functions the game spends nearly all its
// time in, from the list in hot/ (addresses, no game code), so the game is
// playable in about a minute. The rest is built in the background, by the
// launcher run again with `--build-full <title>`, and is used from the next
// start. Drawn with Dear ImGui on SDL's renderer (Direct3D 11 on Windows).

#include "android.h"
#include "build.h"
#include "check.h"
#include "look.h"
#include "pages.h"

#include <SDL3/SDL.h>
#ifndef __ANDROID__
#include <SDL3/SDL_main.h>
#endif
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <mutex>

#include "runtime/decomp_options.h"

namespace launcher {

namespace {

// a file the dialog picked, handed over from SDL's callback
struct Picked {
    std::mutex lock;
    std::string path;
    int which = 0; // 1 the game, 2 the update
    bool ready = false;
};
Picked picked;

void SDLCALL on_picked(void *which, const char *const *files, int) {
    std::lock_guard<std::mutex> hold(picked.lock);
    if (files && files[0]) {
        picked.path = files[0];
        picked.which = int(reinterpret_cast<intptr_t>(which));
        picked.ready = true;
    }
}

enum class Page { Home, Game, Update, Build };
enum class Tab { Home, Graphics, Controls, Advanced };

ImVec4 color(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void text_colored(ImU32 c, const std::string &text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

class App {
public:
    App(SDL_Window *window, Library library, std::optional<Tools> tools)
        : window_(window), library_(std::move(library)), tools_(std::move(tools)) {
        const Port &p = port();
        selected_ = !library_.last.empty() ? library_.last : p.games.empty() ? "" : upper(p.games.front().title);
        pages_.load();
        // the first time, its questions
        if (library_.ready().empty()) set_up(nullptr);
    }

    bool event(const SDL_Event &e) { return pages_.event(e); }
    bool busy() { return job_ && !job_->stage().finished(); }
    bool closing() const { return close_; }

    void frame();

private:
    // the questions again, for a game set up or about to be
    void set_up(const Config *config) {
        game_path_ = config ? config->game : fs::path();
        game_.reset();
        game_error_.clear();
        if (!game_path_.empty()) check_game_file();
        update_path_ = config ? config->update : fs::path();
        update_.reset();
        update_error_.clear();
        if (!update_path_.empty()) check_update_file();
        chosen_decomp_ = config ? config->decomp : fs::path();
        job_.reset();
        note_.clear();
        page_ = Page::Game;
        tab_ = Tab::Home;
    }

    void check_game_file() {
        Game game;
        game_.reset();
        if (check_game(port(), game_path_, game, game_error_)) game_ = game, game_error_.clear();
    }

    void check_update_file() {
        Update update;
        update_.reset();
        if (game_ && check_update(port(), update_path_, *game_, update, update_error_)) update_ = update, update_error_.clear();
    }

    // the game being set up, as a config: built, or not yet
    Config config(bool built) const {
        Config c;
        c.title = game_ ? game_->title() : "";
        c.game = game_path_;
        c.update = update_ ? update_path_ : fs::path();
        c.built = built;
        c.full = built && hot_list(c.title).empty();
        c.decomp = chosen_decomp_;
        if (built) {
            fs::path dir = c.decomp_dir();
            c.decomp_built = dir.empty() ? std::nullopt : fingerprint(dir);
        }
        return c;
    }

    // the color of what is on screen: the game being set up, else the one chosen
    ImU32 accent() const { return look::accent(page_ != Page::Home && game_ ? game_->title() : selected_); }
    const GameInfo *selected_info() const { return port().game(selected_); }

    void picked_files();
    void home(ImVec2 min, ImVec2 max);
    void name_over_glow(ImVec2 min, ImVec2 max);
    void tabs(ImVec2 min, ImVec2 max);
    void steps();
    void game_page();
    void update_page();
    void build_page();
    void advanced();
    void fast_option();
    void open_dialog(int which);

    SDL_Window *window_;
    Library library_;
    std::optional<Tools> tools_;
    PlayerPages pages_;
    // the port's options for the game chosen, read from its code's
    // decomp.txt when the choice changes
    std::vector<runtime::DecompOption> options_;
    std::optional<std::string> options_for_;
    const std::vector<runtime::DecompOption> &options() {
        if (options_for_ != selected_) {
            options_for_ = selected_;
            auto found = library_.games.find(selected_);
            fs::path dir = found != library_.games.end() ? found->second.decomp_dir() : Config{selected_}.decomp_dir();
            options_ = dir.empty() ? std::vector<runtime::DecompOption>() : runtime::read_options(utf8(dir));
        }
        return options_;
    }
    Tab tab_ = Tab::Home;
    Page page_ = Page::Home;
    std::string selected_;
    fs::path game_path_, update_path_, chosen_decomp_;
    std::optional<Game> game_;
    std::optional<Update> update_;
    std::string game_error_, update_error_;
    std::unique_ptr<Job> job_;
    std::string note_;
    bool close_ = false;
};

void App::open_dialog(int which) {
    static const SDL_DialogFileFilter games[] = {{"3DS game", "3ds;cci;cxi;cia"}};
    static const SDL_DialogFileFilter updates[] = {{"3DS update", "cia"}};
#ifdef __ANDROID__
    // Android's picker knows no .3ds or .cia (no type of its own): any file
    SDL_ShowOpenFileDialog(on_picked, reinterpret_cast<void *>(intptr_t(which)), window_, nullptr, 0, nullptr, false);
    (void)games, (void)updates;
#else
    SDL_ShowOpenFileDialog(on_picked, reinterpret_cast<void *>(intptr_t(which)), window_, which == 1 ? games : updates, 1, nullptr, false);
#endif
}

void App::picked_files() {
    std::string path;
    int which;
    {
        std::lock_guard<std::mutex> hold(picked.lock);
        if (!picked.ready) return;
        picked.ready = false;
        path = picked.path, which = picked.which;
    }
    // Android's document (content://) opened as a file this program reads
    path = local_path(path);
    if (which == 1) {
        game_path_ = path_of(path);
        check_game_file();
        // an update chosen for another game no longer fits; the game's own
        // set up before comes back
        update_path_.clear();
        update_.reset();
        update_error_.clear();
        chosen_decomp_.clear();
        if (game_) {
            selected_ = game_->title();
            if (auto known = library_.games.find(game_->title()); known != library_.games.end()) {
                update_path_ = known->second.update;
                chosen_decomp_ = known->second.decomp;
                if (!update_path_.empty()) check_update_file();
            }
        }
    } else if (which == 2) {
        update_path_ = path_of(path);
        check_update_file();
    }
}

void App::home(ImVec2 min, ImVec2 max) {
    const float s = look::scale();
    const Port &p = port();
    const look::Fonts &f = look::fonts();
    const ImU32 acc = accent();
    auto found = library_.games.find(selected_);
    const Config *config = found == library_.games.end() ? nullptr : &found->second;
    const bool ready = config && config->ready();
    const GameInfo *info = selected_info();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float right = max.x - 36 * s;
    float y = max.y - 26 * s;

    // the port's name small, top left
    draw->AddText(f.title22, f.title22->FontSize, ImVec2(min.x + 28 * s, min.y + 22 * s), acc, p.name.c_str());

    // from the bottom up: the hint, the settings, the notes, Play, the games, the name
    auto text_right = [&](ImFont *font, ImU32 c, const std::string &text) {
        ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text.c_str());
        y -= size.y;
        draw->AddText(font, font->FontSize, ImVec2(right - size.x, y), c, text.c_str());
    };
    text_right(f.small, look::WEAK, "In the game: F1 opens the player's menu, F11 switches fullscreen.");
    y -= 4 * s;
    text_right(f.body, look::TEXT, pages_.summary());
    y -= 10 * s;

    std::vector<std::pair<std::string, ImU32>> chips;
    if (config && config->built) {
        std::error_code ec;
        if (config->full) chips.emplace_back("Fully built", look::GOOD);
        else if (fs::exists(lock_file(config->title), ec)) chips.emplace_back("The rest building in the background", look::WARN);
        else chips.emplace_back("The rest builds while you play", look::WARN);
        if (!config->update.empty())
            chips.emplace_back(info && !info->update_name.empty() ? "Update " + info->update_name : "Update", look::GOOD);
        else chips.emplace_back("No update", look::WARN);
        if (!ready) chips.emplace_back("Its files moved", look::BAD);
        else if (!config->current()) chips.emplace_back("The port's code changed: slower until rebuilt", look::WARN);
        if (!config->decomp_dir().empty()) chips.emplace_back("The port's own code", look::GOOD);
    } else {
        chips.emplace_back("Not set up yet", look::WEAK);
    }
    const float chip_h = f.chip->FontSize + 6 * s;
    y -= chip_h;
    float x = right;
    for (auto &[text, c] : chips) {
        x -= look::chip_width(text.c_str());
        look::chip(ImVec2(x, y), text.c_str(), c);
        x -= 8 * s;
    }
    y -= 12 * s;

    const ImVec2 big = look::big_button_size();
    y -= big.y;
    ImGui::PushFont(f.body);
    if (ready) {
        if (look::big_button("play", "PLAY", ImVec2(right - big.x, y), acc)) {
            if (!tools_) note_ = "3dsrecomp or the game's player is missing";
            else if (play(*tools_, *config, note_)) close_ = true;
        }
        const char *change = "Change files";
        ImVec2 size(ImGui::CalcTextSize(change).x + 24 * s, ImGui::GetFrameHeight());
        ImGui::SetCursorScreenPos(ImVec2(right - big.x - 10 * s - size.x, y + (big.y - size.y) / 2));
        if (ImGui::Button(change, size)) set_up(config);
    } else if (look::big_button("setup", "SET UP", ImVec2(right - big.x, y), acc)) {
        set_up(config);
    }
    if (!note_.empty()) {
        ImVec2 size = ImGui::CalcTextSize(note_.c_str());
        draw->AddText(ImVec2(right - size.x, y + big.y + 2 * s), look::BAD, note_.c_str());
    }
    y -= 14 * s;

    // the games of the port, the chosen one in its color
    ImGui::PushFont(f.game);
    const float pill_h = ImGui::GetFrameHeight() + 4 * s;
    y -= pill_h;
    x = right;
    for (auto g = p.games.rbegin(); g != p.games.rend(); ++g) {
        const std::string title = upper(g->title);
        const bool chosen = title == selected_;
        const ImU32 c = look::accent(title);
        const float w = ImGui::CalcTextSize(g->name.c_str()).x + 28 * s;
        x -= w;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, pill_h / 2);
        ImGui::PushStyleColor(ImGuiCol_Button, chosen ? look::alpha(c, 0.85f) : look::alpha(look::INK, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chosen ? c : look::HOVER);
        ImGui::PushStyleColor(ImGuiCol_Border, chosen ? c : look::LINE);
        ImGui::PushStyleColor(ImGuiCol_Text, chosen ? look::rgb(0xFFFFFF) : look::WEAK);
        if (ImGui::Button((g->name + "##" + title).c_str(), ImVec2(w, pill_h))) {
            selected_ = title;
            note_.clear();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        x -= 8 * s;
    }
    ImGui::PopFont();
    ImGui::PopFont();
    y -= 4 * s;
    std::string name = upper(p.name);
    ImVec2 size = f.title76->CalcTextSizeA(f.title76->FontSize, FLT_MAX, 0, name.c_str());
    draw->AddText(f.title76, f.title76->FontSize, ImVec2(right - size.x, y - size.y), look::TEXT, name.c_str());
}

// beside a page: the port's name and the game's, over the glow
void App::name_over_glow(ImVec2, ImVec2 max) {
    const float s = look::scale();
    const look::Fonts &f = look::fonts();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float right = max.x - 36 * s;
    float y = max.y - 30 * s;
    if (const GameInfo *g = port().game(page_ != Page::Home && game_ ? game_->title() : selected_)) {
        ImVec2 size = f.large->CalcTextSizeA(f.large->FontSize, FLT_MAX, 0, g->name.c_str());
        y -= size.y;
        draw->AddText(f.large, f.large->FontSize, ImVec2(right - size.x, y), accent(), g->name.c_str());
    }
    std::string name = upper(port().name);
    ImVec2 size = f.title56->CalcTextSizeA(f.title56->FontSize, FLT_MAX, 0, name.c_str());
    draw->AddText(f.title56, f.title56->FontSize, ImVec2(right - size.x, y - size.y - 4 * s), look::TEXT, name.c_str());
}

void App::fast_option() {
    look::row("Fast compile",
              "For modest processors. The game still plays after about a minute; what is built in the background then takes about a "
              "third of the time, the code the game nearly never runs left unoptimized.",
              ImGui::GetFrameHeight(), [&] {
                  if (ImGui::Checkbox("##fast", &library_.fast)) {
                      std::string error;
                      if (!library_.save(error)) note_ = error;
                  }
              });
}

void App::advanced() {
    const float s = look::scale();
    const Port &p = port();
    const ImU32 acc = accent();
    look::page_title("Advanced", "Building the games, recording, and where things are kept.");
    look::section("Building", acc);
    fast_option();
    const GameInfo *info = selected_info();
    const std::string name = info ? info->name : "the game";
    if (auto found = library_.games.find(selected_); found != library_.games.end() && found->second.built) {
        const Config config = found->second;
        if (!config.full) {
            look::row(("Build all of " + name + " now").c_str(), "Rather than while you play: takes a while, the computer stays usable.",
                      120 * s, [&] {
                          if (ImGui::Button("Build", ImVec2(120 * s, 0))) {
                              finish_in_background(config);
                              note_ = "Building in the background.";
                          }
                      });
        }
        look::row((name + "'s files").c_str(), "Another dump or update, and the game recompiled for them.", 120 * s, [&] {
            if (ImGui::Button("Change", ImVec2(120 * s, 0))) set_up(&config);
        });
    }

    look::section("Recording", acc);
    look::row("Record my input",
              "Each start records what you press, with a copy of the saves as they were, into recordings\\ in the port's folder: for "
              "reporting a problem the way it happened.",
              ImGui::GetFrameHeight(), [&] {
                  bool record = pages_.record();
                  if (ImGui::Checkbox("##record", &record)) pages_.set_record(record);
              });

    look::section("Files", acc);
    look::row("The port's folder",
              "Your saves (user\\), the games as built, the settings. A new version of the port, unzipped over this one or anywhere "
              "else, keeps them.",
              120 * s, [&] {
                  if (ImGui::Button("Open", ImVec2(120 * s, 0))) {
                      std::error_code ec;
                      fs::create_directories(data_dir(), ec);
                      SDL_OpenURL(("file:///" + utf8(data_dir())).c_str());
                  }
              });
#ifndef __ANDROID__
    look::row("Desktop shortcut", "", 120 * s, [&] {
        if (ImGui::Button("Add", ImVec2(120 * s, 0))) {
            std::string error;
            note_ = desktop_shortcut(error) ? "The shortcut is on the desktop." : error;
        }
    });
#endif
    if (!note_.empty()) text_colored(look::WEAK, note_);

    look::section("About", acc);
    if (!p.about.empty()) ImGui::TextWrapped("%s", p.about.c_str());
    text_colored(look::WEAK, p.name + " · " + SDL_GetPlatform());
    ImGui::Dummy(ImVec2(0, 20 * s));
}

void App::steps() {
    const float s = look::scale();
    const char *names[] = {"1 Game", "2 Update", "3 Recompile"};
    const int current = int(page_) - 1;
    ImGui::Dummy(ImVec2(0, 18 * s));
    for (int i = 0; i < 3; i++) {
        if (i) {
            ImGui::SameLine();
            ImGui::TextColored(color(look::WEAK), "›");
            ImGui::SameLine();
        }
        ImGui::TextColored(color(i == current ? accent() : look::WEAK), "%s", names[i]);
    }
}

void App::game_page() {
    const float s = look::scale();
    const Port &p = port();
    std::string names;
    for (size_t i = 0; i < p.games.size(); i++) names += (i ? " or " : "") + p.games[i].name;
    look::page_title("Your game", (p.name + " runs your own copy of " + names +
                                   ", recompiled for this computer. Pick your decrypted dump: .3ds, .cci, .cxi or .cia.").c_str());
    ImGui::Dummy(ImVec2(0, 8 * s));
    if (ImGui::Button("Choose the game…")) open_dialog(1);
    if (!game_path_.empty()) text_colored(look::WEAK, utf8(game_path_));
    if (game_) text_colored(look::GOOD, game_->name);
    else if (!game_error_.empty()) text_colored(look::BAD, game_error_);
    ImGui::Dummy(ImVec2(0, 12 * s));
    if (!library_.ready().empty()) {
        if (ImGui::Button("Back")) page_ = Page::Home;
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!game_);
    if (ImGui::Button("Next")) page_ = Page::Update;
    ImGui::EndDisabled();
}

void App::update_page() {
    if (!game_) {
        page_ = Page::Game;
        return;
    }
    const float s = look::scale();
    const Port &p = port();
    const Game &g = *game_;
    std::string about = "Pick the decrypted update for " + g.name + " (.cia).";
    if (!g.tested_name.empty()) about += " " + p.name + " is tested with " + g.tested_name + ".";
    look::page_title("Its update", about.c_str());
    ImGui::Dummy(ImVec2(0, 8 * s));
    if (ImGui::Button("Choose the update…")) open_dialog(2);
    if (!update_path_.empty()) text_colored(look::WEAK, utf8(update_path_));
    if (update_ && update_->tested(g)) text_colored(look::GOOD, update_->name(g));
    else if (update_)
        text_colored(look::WARN, "This is " + update_->name(g) + ". It should work, but " + p.name + " is tested with " +
                                     (g.tested_name.empty() ? "another" : g.tested_name) + ".");
    else if (!update_error_.empty()) text_colored(look::BAD, update_error_);
    else text_colored(look::WARN, "Without the update the game runs as it came on the cartridge.");
    ImGui::Dummy(ImVec2(0, 12 * s));
    if (ImGui::Button("Back")) page_ = Page::Game;
    ImGui::SameLine();
    // an invalid update has to be replaced or cleared first
    const bool invalid = !update_ && !update_error_.empty();
    ImGui::BeginDisabled(invalid);
    if (ImGui::Button(update_ ? "Next" : "Continue without it")) page_ = Page::Build;
    ImGui::EndDisabled();
    if (invalid) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) update_path_.clear(), update_error_.clear();
    }
}

void App::build_page() {
    const float s = look::scale();
    const Port &p = port();
    const fs::path hot = game_ ? hot_list(game_->title()) : fs::path();
    look::page_title("Recompiling",
                     (hot.empty() ? p.name + " now turns the game's code into code for this computer. It takes around ten to fifteen minutes "
                                             "and only happens once. If there is no C compiler here, one (Zig) is downloaded first."
                                  : p.name + " now turns the game's code into code for this computer, starting with the parts the game spends "
                                             "nearly all its time in: about a minute, longer on slower computers. The rest is built in the "
                                             "background while you play. If there is no C compiler here, one (Zig) is downloaded first.")
                         .c_str());
    ImGui::Dummy(ImVec2(0, 6 * s));
    fast_option();
    ImGui::Dummy(ImVec2(0, 6 * s));
    if (!tools_) {
        text_colored(look::BAD, p.name + " can't find 3dsrecomp and " + p.player + ". It looked in:");
        for (const fs::path &place : places("3dsrecomp")) text_colored(look::WEAK, utf8(place));
        for (const fs::path &place : game_places()) text_colored(look::WEAK, utf8(place));
        ImGui::TextWrapped("Keep the 3dsrecomp and game folders from the download beside the launcher.");
        if (ImGui::Button("Back")) page_ = Page::Update;
        return;
    }
    if (!job_) {
        if (ImGui::Button("Back")) page_ = Page::Update;
        ImGui::SameLine();
        if (ImGui::Button("Start")) job_ = Job::start(*tools_, config(false), hot, false);
        return;
    }
    const Stage stage = job_->stage();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent());
    ImGui::ProgressBar(stage.fraction(), ImVec2(-1, 0));
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", stage.describe().c_str());
    ImGui::Dummy(ImVec2(0, 8 * s));
    if (stage.kind == Stage::Done) {
        const bool fast = library_.fast;
        library_ = Library::load();
        library_.fast = fast;
        Config c = config(true);
        selected_ = c.title;
        library_.put(c);
        std::string error;
        if (!library_.save(error)) note_ = error;
        job_.reset();
        page_ = Page::Home;
    } else if (stage.kind == Stage::Failed) {
        if (ImGui::Button("Try again")) job_.reset();
    } else if (ImGui::Button("Cancel")) {
        job_->cancel();
    }
}

void App::tabs(ImVec2 min, ImVec2 max) {
    const float s = look::scale();
    const look::Fonts &f = look::fonts();
    const ImU32 acc = accent();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, look::alpha(look::INK, 0.92f));
    draw->AddLine(min, ImVec2(max.x, min.y), look::alpha(acc, 0.7f), s);
    float x = min.x + 12 * s;
    const std::pair<Tab, const char *> all[] = {{Tab::Home, "Home"}, {Tab::Graphics, "Graphics"}, {Tab::Controls, "Controls"}, {Tab::Advanced, "Advanced"}};
    ImGui::PushFont(f.title17);
    for (auto [tab, name] : all) {
        const bool chosen = tab_ == tab;
        const float w = std::max(96 * s, ImGui::CalcTextSize(name).x + 28 * s);
        ImGui::SetCursorScreenPos(ImVec2(x, min.y));
        if (ImGui::InvisibleButton(name, ImVec2(w, max.y - min.y)) && !chosen) {
            tab_ = tab;
            pages_.load(); // the player may have changed them in the game
            if (page_ != Page::Home && !job_ && tab != Tab::Home) page_ = Page::Home;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImVec2 size = ImGui::CalcTextSize(name);
        draw->AddText(ImVec2(x + (w - size.x) / 2, min.y + (max.y - min.y - size.y) / 2), chosen ? acc : hovered ? look::TEXT : look::WEAK, name);
        if (chosen) draw->AddRectFilled(ImVec2(x, max.y - 3 * s), ImVec2(x + w, max.y), acc);
        x += w;
    }
    ImGui::PopFont();
    const std::string version = port().name;
    ImVec2 size = f.small->CalcTextSizeA(f.small->FontSize, FLT_MAX, 0, version.c_str());
    draw->AddText(f.small, f.small->FontSize, ImVec2(max.x - 14 * s - size.x, min.y + (max.y - min.y - size.y) / 2), look::WEAK, version.c_str());
}

void App::frame() {
    picked_files();
    const float s = look::scale();
    const ImGuiViewport *view = ImGui::GetMainViewport();
    const ImVec2 min = view->Pos, max(view->Pos.x + view->Size.x, view->Pos.y + view->Size.y);
    const ImU32 acc = accent();
    look::background(ImGui::GetBackgroundDrawList(), min, max, acc, look::partner(selected_));

    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float bar = 48 * s;
    const ImVec2 content_max(max.x, max.y - bar);
    const bool paged = page_ != Page::Home || tab_ != Tab::Home;
    if (paged) {
        const float width = std::clamp(view->Size.x * 0.5f, 480 * s, 640 * s);
        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, ImVec2(min.x + width, content_max.y), look::alpha(look::PANEL, 0.97f));
        draw->AddLine(ImVec2(min.x + width, min.y), ImVec2(min.x + width, content_max.y), look::alpha(acc, 0.6f), s);
        name_over_glow(ImVec2(min.x + width, min.y), content_max);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * s, 0));
        ImGui::BeginChild("page", ImVec2(width, content_max.y - min.y), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (page_ != Page::Home) {
            steps();
            if (page_ == Page::Game) game_page();
            else if (page_ == Page::Update) update_page();
            else build_page();
        } else if (tab_ == Tab::Graphics) {
            pages_.graphics(acc, options());
        } else if (tab_ == Tab::Controls) {
            pages_.controls(acc);
        } else {
            advanced();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        home(min, content_max);
    }
    tabs(ImVec2(min.x, content_max.y), max);
    ImGui::End();
}

} // namespace

} // namespace launcher

using namespace launcher;

#ifdef __ANDROID__
// Android: frontend/android_main.cpp runs this, then the player, in one
// program (libmain.so)
int launcher_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    set_args(argc, argv);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL: %s", SDL_GetError());
        return 1;
    }
    std::string error;
    const Port *p = get_port(&error);
    if (!p) {
        // without its port.toml the launcher knows no game
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "This launcher has no port.toml",
                                 (error + "\n\nKeep port.toml from the download beside the launcher.").c_str(), nullptr);
        return 1;
    }
    Library library = Library::load();
    std::optional<Tools> tools = Tools::find(library);
    if (has_arg("--build-full")) {
        if (auto title = arg_after("--build-full")) build_full(*title);
        return 0;
    }
    // straight into the game, from a terminal or a shortcut: the launcher
    // otherwise always opens first
    if (has_arg("--play")) {
        Config config = library.current();
        if (auto title = arg_after("--play"); title && library.games.count(upper(*title))) config = library.games[upper(*title)];
        if (config.ready() && tools && play(*tools, config, error)) return 0;
    }

    SDL_Window *window = SDL_CreateWindow(p->name.c_str(), 1180, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    const float scale = window ? SDL_GetWindowDisplayScale(window) : 1;
    // the port's icon on the window where the program has none of its own
    // (beyond Windows, whose .exe carries it): icon.bmp beside the launcher
    if (std::error_code ec; window && fs::is_regular_file(beside("icon.bmp"), ec)) {
        if (SDL_Surface *icon = SDL_LoadBMP(utf8(beside("icon.bmp")).c_str())) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    }
    // the first renderer that starts, Vulkan never: NVIDIA's driver has been
    // seen to crash presenting a window with it (SDL_RENDER_DRIVER still chooses)
    SDL_Renderer *renderer = nullptr;
    const char *chosen = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    for (const char *driver : {"direct3d11", "direct3d12", "metal", "opengl", "opengles2", "software"}) {
        if (!window || renderer) break;
        renderer = SDL_CreateRenderer(window, chosen && *chosen ? chosen : driver);
        if (chosen && *chosen) break;
    }
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, p->name.c_str(), SDL_GetError(), nullptr);
        return 1;
    }
    // the size in the display's pixels, as a window of 1180 by 720 at its scale
    SDL_SetWindowSize(window, int(1180 * scale), int(720 * scale));
    SDL_SetWindowMinimumSize(window, int(960 * scale), int(600 * scale));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetRenderVSync(renderer, 1);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // the keyboard moves between the controls; a gamepad only when one is
    // being bound (a virtual one, as Parsec's, drifts)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    look::install(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app(window, std::move(library), std::move(tools));
    bool running = true;
    int idle = 0;
    while (running && !app.closing()) {
        // waits for something to happen, a while on, sooner while building
        SDL_Event e;
        bool any = SDL_WaitEventTimeout(&e, idle > 2 ? (app.busy() ? 250 : 1000) : 16);
        while (any) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window)))
                running = false;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
            if (!app.event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            idle = 0;
            any = SDL_PollEvent(&e);
        }
        idle++;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 16, 17, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#ifndef __ANDROID__
    SDL_Quit(); // on Android the player comes next, in this program
#endif
    return 0;
}
