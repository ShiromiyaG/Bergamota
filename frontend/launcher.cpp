#include "ui.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <mutex>

namespace frontend {

void ui_init(SDL_Window *window, SDL_Renderer *renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing beside config.toml
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    float scale = SDL_GetWindowDisplayScale(window);
    if (scale > 0) {
        ImGui::GetStyle().ScaleAllSizes(scale);
        io.FontGlobalScale = scale;
    }
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
}

void ui_shutdown() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

namespace {

// a file the dialog picked, handed over from SDL's callback
struct Picked {
    std::mutex lock;
    std::string *into = nullptr;
    std::string path;
    bool ready = false;
};
Picked picked;

void SDLCALL on_picked(void *, const char *const *files, int) {
    std::lock_guard guard(picked.lock);
    if (files && files[0]) {
        picked.path = files[0];
        picked.ready = true;
    }
}

// a path with a button that opens the system's dialog for it
void path_row(SDL_Window *window, const char *label, std::string &value, bool folder,
              const SDL_DialogFileFilter *filter = nullptr) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    char buffer[1024];
    SDL_strlcpy(buffer, value.c_str(), sizeof buffer);
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##path", buffer, sizeof buffer)) value = buffer;
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
        {
            std::lock_guard guard(picked.lock);
            picked.into = &value;
            picked.ready = false;
        }
        if (folder) SDL_ShowOpenFolderDialog(on_picked, nullptr, window, value.empty() ? nullptr : value.c_str(), false);
        else SDL_ShowOpenFileDialog(on_picked, nullptr, window, filter, filter ? 1 : 0, nullptr, false);
    }
    ImGui::PopID();
}

} // namespace

bool launcher(SDL_Window *window, SDL_Renderer *renderer, Settings &s, const std::filesystem::path &file) {
    static const SDL_DialogFileFilter EXHEADER{"Exheader", "bin"}, LIBRARY{"Recompiled library", "dll;so;dylib"},
        GAME{"Game", "3ds;cci;cia;cxi"}, UPDATE{"Update", "cia"};
    std::string message;
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) return false;
        }
        {
            std::lock_guard guard(picked.lock);
            if (picked.ready && picked.into) *picked.into = picked.path;
            picked.ready = false;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *view = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(view->WorkPos);
        ImGui::SetNextWindowSize(view->WorkSize);
        ImGui::Begin("Launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::SeparatorText("Game");
        path_row(window, "Game (.3ds or .cia)", s.game.game, false, &GAME);
        path_row(window, "Update (.cia, optional)", s.game.update, false, &UPDATE);
        path_row(window, "Exheader (exheader.bin)", s.game.exheader, false, &EXHEADER);
        path_row(window, "Code (code.bin)", s.game.code, false, &EXHEADER);
        path_row(window, "Recompiled library", s.game.library, false, &LIBRARY);
        path_row(window, "Saves folder", s.game.user, true);

        ImGui::SeparatorText("Video");
        ImGui::Checkbox("Fullscreen", &s.video.fullscreen);
        ImGui::TextDisabled("The rest (filter, layout, volume, controls) is in the overlay: F1 in the game.");

        ImGui::Separator();
        bool ready = s.game_ready();
        ImGui::BeginDisabled(!ready);
        bool start = ImGui::Button("Start", ImVec2(160, 0));
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool quit = ImGui::Button("Quit", ImVec2(160, 0));
        if (!ready) ImGui::TextDisabled("The game, its exheader, code and library, and a saves folder are needed.");
        if (!message.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", message.c_str());
        ImGui::TextDisabled("Settings: %s", utf8(file).c_str());
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(10); // a form, not a game

        if (quit) return false;
        if (start) {
            std::string error;
            if (save(file, s, error)) return true;
            message = error;
        }
    }
}

} // namespace frontend
