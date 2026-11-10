// Scrolling a Dear ImGui window with a finger, on a phone: dragged up or
// down anywhere over it, not only by its scrollbar, and coasting on a little
// once let go. Elsewhere it does nothing.
#ifndef FRONTEND_DRAG_SCROLL_H
#define FRONTEND_DRAG_SCROLL_H

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>

namespace frontend {

// inside the window to scroll (after its Begin or BeginChild), every frame.
// A drag that starts mostly sideways is left to what it went down on (a
// slider); one up or down takes the press away from it (a button dragged
// over is not pressed)
inline void drag_scroll() {
#if defined(__ANDROID__) || defined(DRAG_SCROLL_ALWAYS)
    static ImGuiID dragged = 0, coasting = 0;
    static float speed = 0;
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    const ImGuiIO &io = g.IO;
    const bool down = io.MouseDown[0];
    if (dragged == window->ID) {
        if (down) {
            speed = -io.MouseDelta.y;
            ImGui::SetScrollY(window->Scroll.y + speed);
        } else {
            dragged = 0;
            coasting = window->ID;
        }
        return;
    }
    if (down) {
        if (coasting == window->ID) coasting = 0; // a finger stops it
        if (dragged || window->ScrollMax.y <= 0) return;
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
        const float x = io.MousePos.x - io.MouseClickedPos[0].x, y = io.MousePos.y - io.MouseClickedPos[0].y;
        if (std::fabs(y) <= io.MouseDragThreshold || std::fabs(y) <= std::fabs(x)) return;
        dragged = window->ID;
        speed = 0;
        ImGui::ClearActiveID();
        ImGui::SetScrollY(window->Scroll.y - io.MouseDelta.y);
    } else if (coasting == window->ID) {
        speed *= 0.93f;
        if (std::fabs(speed) < 0.5f) coasting = 0;
        else ImGui::SetScrollY(window->Scroll.y + speed);
    }
#endif
}

} // namespace frontend

#endif
