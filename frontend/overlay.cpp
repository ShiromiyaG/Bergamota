#include "ui.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

namespace frontend {

bool Overlay::event(const SDL_Event &event) {
    // a button waiting for its binding takes the next key or gamepad button
    if (binding_) {
        if (!binding_pad_ && event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.scancode != SDL_SCANCODE_ESCAPE) pending_ = SDL_GetScancodeName(event.key.scancode);
            done_ = true;
            return true;
        }
        if (binding_pad_ && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            pending_ = SDL_GetGamepadStringForButton(SDL_GamepadButton(event.gbutton.button));
            done_ = true;
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F1 && !event.key.repeat) {
        open = !open;
        return true;
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
    return open && (ImGui::GetIO().WantCaptureKeyboard || ImGui::GetIO().WantCaptureMouse);
}

bool Overlay::keyboard() const { return open && ImGui::GetIO().WantCaptureKeyboard; }

void Overlay::draw(Settings &s, const Stats &stats, const std::filesystem::path &file, bool &changed) {
    if (binding_ && done_) {
        if (!pending_.empty()) {
            (binding_pad_ ? s.pad : s.keys)[binding_] = pending_;
            changed = dirty_ = true;
        }
        binding_ = nullptr;
        done_ = false;
        pending_.clear();
    }
    if (s.stats) {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##stats", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%d fps, worst %.0f ms", stats.shown, stats.worst_ms);
        ImGui::Text("gpu %.0f ms, idle %.0f ms", stats.gpu_ms, stats.idle_ms);
        ImGui::End();
    }
    if (!open) {
        if (dirty_) {
            // saved once the overlay closes
            std::string error;
            if (save(file, s, error)) dirty_ = false;
        }
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool keep = true;
    ImGui::Begin("Settings (F1)", &keep, ImGuiWindowFlags_NoSavedSettings);
    bool moved = false;
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Video")) {
            moved |= ImGui::Checkbox("Fullscreen (F11)", &s.video.fullscreen);
            moved |= ImGui::Checkbox("Smooth scaling", &s.video.smooth);
            moved |= ImGui::Checkbox("60 FPS (frames drawn between the game's)", &s.video.between);
            if (s.video.between) moved |= ImGui::Checkbox("Up to the display's rate (120, 144 Hz)", &s.video.between_display);
            moved |= ImGui::Checkbox("Vertical sync (can cost speed)", &s.video.vsync);
            int layout = s.video.layout == "top" ? 1 : 0;
            if (ImGui::Combo("Screens", &layout, "Both, stacked\0Top only\0")) {
                s.video.layout = layout ? "top" : "both";
                moved = true;
            }
            int aspect = s.video.aspect == "16:9" ? 1 : s.video.aspect == "21:9" ? 2 : 0;
            if (ImGui::Combo("Top screen", &aspect, "Original (5:3)\0Widescreen (16:9)\0Ultrawide (21:9)\0")) {
                s.video.aspect = aspect == 1 ? "16:9" : aspect == 2 ? "21:9" : "original";
                moved = true;
            }
            static const char *RESOLUTIONS[] = {"native", "1280x720", "1920x1080", "2560x1440", "3840x2160", "window"};
            int resolution = 0;
            for (int i = 0; i < 6; i++) {
                if (s.video.resolution == RESOLUTIONS[i]) resolution = i;
            }
            if (ImGui::Combo("Resolution", &resolution,
                             "Native (400x240)\0" "720p\0" "1080p\0" "1440p\0" "4K\0" "The window's\0")) {
                s.video.resolution = RESOLUTIONS[resolution];
                moved = true;
            }
            moved |= ImGui::Checkbox("Anti-aliasing (FXAA)", &s.video.antialias);
            int outlines = s.video.outlines == "thin" ? 1 : s.video.outlines == "none" ? 2 : 0;
            if (ImGui::Combo("Outlines", &outlines, "As the game draws them\0Thin\0None\0")) {
                s.video.outlines = outlines == 1 ? "thin" : outlines == 2 ? "none" : "original";
                moved = true;
            }
            moved |= ImGui::SliderFloat("Sharpening", &s.video.sharpen, 0.0f, 1.0f, "%.2f");
            moved |= ImGui::Checkbox("Frame times in the corner", &s.stats);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            moved |= ImGui::SliderInt("Volume", &s.audio.volume, 0, 100, "%d%%");
            moved |= ImGui::Checkbox("Mute", &s.audio.mute);
            ImGui::EndTabItem();
        }
        for (int pad = 0; pad < 2; pad++) {
            if (!ImGui::BeginTabItem(pad ? "Gamepad" : "Keyboard")) continue;
            auto &map = pad ? s.pad : s.keys;
            if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_RowBg)) {
                for (const Button &b : BUTTONS) {
                    if (pad && !b.bit) continue; // the left stick is the circle pad
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(b.name);
                    ImGui::TableNextColumn();
                    ImGui::PushID(b.name);
                    bool waiting = binding_ == b.name && binding_pad_ == bool(pad);
                    auto it = map.find(b.name);
                    std::string label = waiting ? "press one (Esc: cancel)" : it == map.end() || it->second.empty() ? "-" : it->second;
                    if (ImGui::Button(label.c_str(), ImVec2(-1, 0)) && !binding_) {
                        binding_ = b.name;
                        binding_pad_ = pad;
                        pending_.clear();
                        done_ = false;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!message_.empty()) ImGui::TextDisabled("%s", message_.c_str());
    ImGui::End();
    if (!keep) open = false;
    if (moved) changed = dirty_ = true;
}

} // namespace frontend
