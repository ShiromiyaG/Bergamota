#include "ui.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_impl_vulkan.h>

#include <mutex>

namespace frontend {

namespace {
enum class Backend { None, SdlRenderer, Vulkan } backend = Backend::None;
} // namespace

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
    ui_on_renderer(window, renderer);
}

void ui_on_renderer(SDL_Window *window, SDL_Renderer *renderer) {
    ui_drop_backend();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    backend = Backend::SdlRenderer;
}

bool ui_on_vulkan(SDL_Window *window, const runtime::VulkanHandles &vk) {
    ui_drop_backend();
    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = vk.instance;
    info.PhysicalDevice = vk.physical;
    info.Device = vk.device;
    info.QueueFamily = vk.family;
    info.Queue = vk.queue;
    info.DescriptorPoolSize = 16; // a pool of its own
    info.MinImageCount = 2;
    info.ImageCount = vk.images < 2 ? 2 : vk.images;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vk.format;
    if (!ImGui_ImplVulkan_Init(&info)) {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }
    backend = Backend::Vulkan;
    return true;
}

void ui_new_frame() {
    if (backend == Backend::Vulkan) ImGui_ImplVulkan_NewFrame();
    else ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

void ui_draw_vulkan(VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); }

void ui_drop_backend() {
    if (backend == Backend::SdlRenderer) ImGui_ImplSDLRenderer3_Shutdown();
    if (backend == Backend::Vulkan) ImGui_ImplVulkan_Shutdown();
    if (backend != Backend::None) ImGui_ImplSDL3_Shutdown();
    backend = Backend::None;
}

void ui_shutdown() {
    ui_drop_backend();
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
        int which = s.video.renderer == "zakuro" ? 1 : 0;
        if (ImGui::Combo("Renderer", &which, "The runtime's (Vulkan)\0Zakuro's (Vulkan, through its bridge)\0")) {
            s.video.renderer = which ? "zakuro" : "native";
        }
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
