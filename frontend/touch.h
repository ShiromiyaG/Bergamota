// The 3DS's controls on a touch screen (Android's phones): the circle pad
// and the D-pad on the left, A, B, X and Y on the right, L and R at the top
// corners, Select and Start at the foot, and a button for the menu (F1).
// Each finger works the control it went down on; a finger on none of them
// is the stylus, on the bottom screen where it is shown.
#ifndef FRONTEND_TOUCH_H
#define FRONTEND_TOUCH_H

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <map>
#include <optional>

namespace frontend {

class TouchControls {
public:
    // drawn and taking fingers: on a device that has a touch screen, until a
    // gamepad is used (and again once none is connected)
    bool shown = false;
    // the menu's button, shown wherever there is a touch screen
    bool menu_button = false;

    // takes a finger event (w, h: the window in its points); true when it
    // went to a control (the game sees it as the control, not a touch)
    bool event(const SDL_Event &e, float w, float h);
    // the buttons held, as the 3DS's bits, and the circle pad, -1 to 1
    uint32_t buttons() const;
    float circle_x() const { return circle_x_; }
    float circle_y() const { return circle_y_; }
    // a finger on none of the controls: where, in the window's points
    std::optional<ImVec2> stylus() const;
    // the menu's button pressed since last asked
    bool menu();
    // drawn over everything, in the window's points: the menu's button, and
    // the controls when controls (not while the menu is open)
    void draw(ImDrawList *draw, float w, float h, bool controls) const;

private:
    enum Control { None, Circle, Pad, A, B, X, Y, L, R, Start, Select, Menu, Stylus };
    struct Finger {
        Control control = None;
        ImVec2 at;
    };
    // where each control is in a window of w by h points
    struct Layout {
        float u;          // a hundredth of the height
        ImVec2 circle, pad, face, menu;
        float circle_r, pad_r, face_r, face_gap;
        ImVec2 l0, l1, r0, r1, select, start;
        ImVec2 pill; // Select's and Start's size
    };
    static Layout layout(float w, float h);
    Control hit(ImVec2 p, float w, float h) const;
    void update(float w, float h);

    std::map<SDL_FingerID, Finger> fingers_;
    uint32_t buttons_ = 0;
    float circle_x_ = 0, circle_y_ = 0;
    bool menu_ = false;
};

} // namespace frontend

#endif
