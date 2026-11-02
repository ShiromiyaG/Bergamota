#include "pages.h"

#include "library.h"
#include "look.h"

#include <SDL3/SDL.h>

#include <utility>

namespace launcher {

namespace {

// the 3DS's buttons as players know them, in the settings' order
const char *const BUTTON_NAMES[] = {"A", "B", "X", "Y", "L", "R", "START", "SELECT", "D-Pad up", "D-Pad down", "D-Pad left",
                                    "D-Pad right", "Circle Pad up", "Circle Pad down", "Circle Pad left", "Circle Pad right"};

// a gamepad button as SDL names it, and as players know it
const char *pad_name(const std::string &button) {
    static const std::pair<const char *, const char *> names[] = {
        {"a", "South (A, Cross)"},     {"b", "East (B, Circle)"},          {"x", "West (X, Square)"},
        {"y", "North (Y, Triangle)"}, {"leftshoulder", "Left bumper (L1)"}, {"rightshoulder", "Right bumper (R1)"},
        {"start", "Start, Menu"},      {"back", "Back, View, Share"},       {"guide", "Guide, Home"},
        {"leftstick", "Left stick press"}, {"rightstick", "Right stick press"}, {"dpup", "D-Pad up"},
        {"dpdown", "D-Pad down"},      {"dpleft", "D-Pad left"},            {"dpright", "D-Pad right"},
    };
    for (auto &[sdl, name] : names)
        if (button == sdl) return name;
    return button.empty() ? "—" : button.c_str();
}

bool choice(const char *id, std::string &value, std::initializer_list<std::pair<const char *, const char *>> options) {
    const char *shown = value.c_str();
    for (auto &[v, name] : options)
        if (value == v) shown = name;
    bool changed = false;
    if (ImGui::BeginCombo(id, shown)) {
        for (auto &[v, name] : options) {
            if (ImGui::Selectable(name, value == v)) {
                value = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

void PlayerPages::load() {
    settings_ = frontend::Settings();
    fs::path path = player_toml();
    std::string error;
    if (!path.empty() && !frontend::load(path, settings_, error)) note_ = error;
}

void PlayerPages::save() {
    fs::path path = player_toml();
    if (path.empty()) {
        note_ = "there is no APPDATA folder";
        return;
    }
    // what the player may have changed meanwhile, then this change over it:
    // the pages hold the whole of the settings, so they win
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::string error;
    note_ = frontend::save(path, settings_, error) ? "" : error;
}

void PlayerPages::set_record(bool on) {
    load();
    settings_.game.record = on;
    save();
}

std::string PlayerPages::summary() const {
    const auto &v = settings_.video;
    std::string layout = v.layout == "single" ? "Single screen" : v.layout == "top" ? "Top screen only" : "Both screens";
    std::string resolution = v.resolution == "native" ? "Console resolution" : v.resolution == "window" ? "Window resolution" : v.resolution;
    if (auto x = resolution.find('x'); x != std::string::npos && v.resolution != "native" && v.resolution != "window")
        resolution.replace(x, 1, " × ");
    return resolution + " · " + layout + " · " + (v.between ? "60 FPS" : "Original frame rate") + " · " +
           (v.fullscreen ? "Fullscreen" : "Windowed");
}

void PlayerPages::graphics(ImU32 accent) {
    const float s = look::scale(), wide = 230 * s, box = ImGui::GetFrameHeight();
    auto &v = settings_.video;
    bool changed = false;
    auto check = [&](const char *id, bool &value) { changed |= ImGui::Checkbox(id, &value); };
    look::page_title("Graphics", "Saved for the next start. In the game, F1 opens the same settings.");
    look::section("Window", accent);
    look::row("Fullscreen", "Covers the screen; F11 switches in the game.", box, [&] { check("##fullscreen", v.fullscreen); });
    look::row("VSync", "Waits for the display: no tearing, a little more latency.", box, [&] { check("##vsync", v.vsync); });

    look::section("Screens", accent);
    look::row("Layout", "Single screen: the top screen, the bottom one as a panel (Tab, L3). L2 swaps them.", wide, [&] {
        changed |= choice("##layout", v.layout, {{"single", "Single screen"}, {"both", "Both screens, stacked"}, {"top", "Top screen only"}});
    });
    if (v.layout == "single") {
        look::row("Panel size", "The bottom screen's panel, against the top screen's height.", wide,
                  [&] { changed |= ImGui::SliderFloat("##panel_size", &v.panel_size, 0.2f, 0.6f, "%.2f"); });
        look::row("Panel corner", "", wide, [&] {
            changed |= choice("##corner", v.panel_corner,
                              {{"bottom-right", "Bottom right"}, {"bottom-left", "Bottom left"}, {"top-right", "Top right"}, {"top-left", "Top left"}});
        });
        look::row("Panel out after cutscenes", "Shows the bottom screen once a movie shown on both is past.", box,
                  [&] { check("##panel_shown", v.panel_shown); });
    }

    look::section("Picture", accent);
    look::row("Resolution", "What the 3D is drawn at; the screens scale to the window.", wide, [&] {
        changed |= choice("##resolution", v.resolution,
                          {{"native", "Console (400 × 240)"}, {"window", "The window's"}, {"1280x720", "1280 × 720"},
                           {"1920x1080", "1920 × 1080"}, {"2560x1440", "2560 × 1440"}, {"3840x2160", "3840 × 2160"}});
    });
    look::row("Aspect ratio", "Wider than the console's where the port's code frames a wider scene.", wide, [&] {
        changed |= choice("##aspect", v.aspect, {{"original", "Original (5:3)"}, {"16:9", "16:9"}, {"21:9", "21:9"}});
    });
    look::row("Outlines", "Auto: the game's at the console's resolution, thin above it.", wide, [&] {
        changed |= choice("##outlines", v.outlines, {{"auto", "Auto"}, {"original", "Original"}, {"thin", "Thin"}, {"none", "None"}});
    });
    look::row("Smooth scaling", "Off: sharp pixels.", box, [&] { check("##smooth", v.smooth); });
    look::row("Anti-aliasing (FXAA)", "", box, [&] { check("##antialias", v.antialias); });
    look::row("HD textures", "The PNGs in the textures folder beside the settings, in place of the game's.", box,
              [&] { check("##hd_textures", v.hd_textures); });
    look::row("Dump textures", "Every texture the game uses written to texture-dump, for making HD ones.", box,
              [&] { check("##dump_textures", v.dump_textures); });
    look::row("Sharpen", "", wide, [&] { changed |= ImGui::SliderFloat("##sharpen", &v.sharpen, 0, 1, "%.2f"); });
    look::row("No bloom ghosting", "Leaves out the trail the bloom draws behind what moves.", box, [&] { check("##ghosting", v.no_ghosting); });

    look::section("Motion", accent);
    look::row("60 FPS", "A frame drawn between each two of the game's, for the scenes it draws at 30.", box, [&] { check("##between", v.between); });
    look::row("Up to the display's rate", "More frames between on 120 and 144 Hz displays.", box,
              [&] { check("##between_display", v.between_display); });

    look::section("Sound", accent);
    look::row("Volume", "", wide, [&] { changed |= ImGui::SliderInt("##volume", &settings_.audio.volume, 0, 100, "%d%%"); });
    look::row("Mute", "", box, [&] { check("##mute", settings_.audio.mute); });
    look::row("Frame times", "A corner of the screen with how long each frame takes.", box, [&] { check("##stats", settings_.stats); });
    // a slider saves once let go
    if (changed && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) save();
    else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) save();
    if (!note_.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(look::BAD), "%s", note_.c_str());
    ImGui::Dummy(ImVec2(0, 20 * s));
}

bool PlayerPages::event(const SDL_Event &e) {
    if (capture_.empty()) return false;
    if (!pad_ && e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.scancode != SDL_SCANCODE_ESCAPE) {
            load();
            settings_.keys[capture_] = SDL_GetScancodeName(e.key.scancode);
            save();
        }
        capture_.clear();
        return true;
    }
    if (pad_ && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (const char *name = SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button))) {
            load();
            settings_.pad[capture_] = name;
            save();
        }
        capture_.clear();
        return true;
    }
    // the keyboard gives up on a gamepad's choice
    if (pad_ && e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
        capture_.clear();
        return true;
    }
    return e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
           e.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
}

void PlayerPages::controls(ImU32 accent) {
    const float s = look::scale();
    look::page_title("Controls", "Which key or gamepad button presses each of the 3DS's buttons.");
    look::row("Device", "", 230 * s, [&] {
        const float half = (230 * s - ImGui::GetStyle().ItemSpacing.x) / 2;
        for (bool pad : {false, true}) {
            if (pad) ImGui::SameLine();
            const bool chosen = pad_ == pad;
            if (chosen) ImGui::PushStyleColor(ImGuiCol_Button, accent);
            if (ImGui::Button(pad ? "Gamepad" : "Keyboard", ImVec2(half, 0)) && !chosen) {
                pad_ = pad;
                capture_.clear();
            }
            if (chosen) ImGui::PopStyleColor();
        }
    });
    ImGui::PushFont(look::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text, look::WEAK);
    ImGui::TextWrapped("On a gamepad the left stick is the Circle Pad. The right stick moves a pointer over the bottom screen, R2 or "
                       "R3 touches, L2 swaps the screens. Click a button, then press the key or gamepad button for it; Escape gives up.");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    look::section(pad_ ? "Gamepad" : "Keyboard", accent);
    const size_t count = pad_ ? 12 : std::size(frontend::BUTTONS); // the Circle Pad is the stick
    auto &map = pad_ ? settings_.pad : settings_.keys;
    for (size_t i = 0; i < count; i++) {
        const std::string button = frontend::BUTTONS[i].name;
        const bool choosing = capture_ == button;
        look::row(BUTTON_NAMES[i], "", 270 * s, [&] {
            auto found = map.find(button);
            std::string now = found == map.end() ? "" : found->second;
            std::string label = choosing ? (pad_ ? "Press a button…" : "Press a key…") : pad_ ? pad_name(now) : now.empty() ? "—" : now;
            if (choosing) ImGui::PushStyleColor(ImGuiCol_Text, accent);
            if (ImGui::Button((label + "##" + button).c_str(), ImVec2(190 * s, 0))) capture_ = choosing ? "" : button;
            if (choosing) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button(("Clear##" + button).c_str(), ImVec2(70 * s, 0))) {
                load();
                (pad_ ? settings_.pad : settings_.keys)[button] = "";
                save();
            }
        });
    }
    ImGui::Dummy(ImVec2(0, 8 * s));
    if (ImGui::Button(pad_ ? "Gamepad defaults" : "Keyboard defaults")) {
        load();
        frontend::Settings defaults;
        (pad_ ? settings_.pad : settings_.keys) = pad_ ? defaults.pad : defaults.keys;
        capture_.clear();
        save();
    }
    if (!note_.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(look::BAD), "%s", note_.c_str());
    ImGui::Dummy(ImVec2(0, 20 * s));
}

} // namespace launcher
