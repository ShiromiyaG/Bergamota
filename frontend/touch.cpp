#include "touch.h"

#include "settings.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace frontend {

namespace {

float distance(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool inside(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; }

// a button's bit by its name, as the settings list them
uint32_t bit(const char *name) {
    for (const Button &b : BUTTONS)
        if (std::string(b.name) == name) return b.bit;
    return 0;
}

} // namespace

TouchControls::Layout TouchControls::layout(float w, float h) {
    Layout l;
    const float u = h / 100;
    l.u = u;
    l.circle_r = 13 * u;
    l.circle = {6 * u + l.circle_r, 50 * u};
    l.pad_r = 11 * u;
    l.pad = {l.circle.x, 82 * u};
    l.face_r = 6.5f * u;
    l.face_gap = 10 * u;
    l.face = {w - 6 * u - l.face_r - l.face_gap, 70 * u};
    l.l0 = {0, 0}, l.l1 = {28 * u, 12 * u};
    l.r0 = {w - 28 * u, 0}, l.r1 = {w, 12 * u};
    l.pill = {13 * u, 6 * u};
    l.select = {w / 2 - 9 * u, h - 5 * u};
    l.start = {w / 2 + 9 * u, h - 5 * u};
    l.menu = {w / 2, 5 * u};
    return l;
}

TouchControls::Control TouchControls::hit(ImVec2 p, float w, float h) const {
    const Layout l = layout(w, h);
    if (menu_button && distance(p, l.menu) < 5 * l.u) return Menu;
    if (!shown) return None;
    // a little past each, for thumbs
    if (distance(p, l.circle) < l.circle_r * 1.3f) return Circle;
    if (distance(p, l.pad) < l.pad_r * 1.3f) return Pad;
    const ImVec2 faces[] = {{l.face.x + l.face_gap, l.face.y}, {l.face.x, l.face.y + l.face_gap}, {l.face.x, l.face.y - l.face_gap},
                            {l.face.x - l.face_gap, l.face.y}};
    const Control names[] = {A, B, X, Y};
    for (int i = 0; i < 4; i++)
        if (distance(p, faces[i]) < l.face_r * 1.35f) return names[i];
    if (inside(p, l.l0, l.l1)) return L;
    if (inside(p, l.r0, l.r1)) return R;
    auto on_pill = [&](ImVec2 c) { return std::abs(p.x - c.x) < l.pill.x * 0.65f && std::abs(p.y - c.y) < l.pill.y; };
    if (on_pill(l.select)) return Select;
    if (on_pill(l.start)) return Start;
    return None;
}

bool TouchControls::event(const SDL_Event &e, float w, float h) {
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION && e.type != SDL_EVENT_FINGER_UP &&
        e.type != SDL_EVENT_FINGER_CANCELED)
        return false;
    const ImVec2 p{e.tfinger.x * w, e.tfinger.y * h};
    bool took = false;
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        Control c = hit(p, w, h);
        if (c == Menu) menu_ = true;
        fingers_[e.tfinger.fingerID] = Finger{c == None ? Stylus : c, p};
        took = c != None;
    } else if (auto it = fingers_.find(e.tfinger.fingerID); it != fingers_.end()) {
        took = it->second.control != Stylus;
        if (e.type == SDL_EVENT_FINGER_MOTION) {
            it->second.at = p;
            // a face button follows the finger to the next one (rolling A to B)
            if (it->second.control >= A && it->second.control <= Y) {
                Control c = hit(p, w, h);
                if (c >= A && c <= Y) it->second.control = c;
            }
        } else {
            fingers_.erase(it);
        }
    }
    update(w, h);
    return took;
}

void TouchControls::update(float w, float h) {
    const Layout l = layout(w, h);
    buttons_ = 0;
    circle_x_ = circle_y_ = 0;
    for (const auto &[id, f] : fingers_) {
        switch (f.control) {
        case Circle: {
            float x = (f.at.x - l.circle.x) / l.circle_r, y = -(f.at.y - l.circle.y) / l.circle_r;
            float m = std::hypot(x, y);
            if (m > 1) x /= m, y /= m;
            circle_x_ = x, circle_y_ = y;
            break;
        }
        case Pad: {
            // eight ways: each axis on past a third of the pad
            float x = (f.at.x - l.pad.x) / l.pad_r, y = (f.at.y - l.pad.y) / l.pad_r;
            if (x < -0.35f) buttons_ |= bit("left");
            if (x > 0.35f) buttons_ |= bit("right");
            if (y < -0.35f) buttons_ |= bit("up");
            if (y > 0.35f) buttons_ |= bit("down");
            break;
        }
        case A: buttons_ |= bit("a"); break;
        case B: buttons_ |= bit("b"); break;
        case X: buttons_ |= bit("x"); break;
        case Y: buttons_ |= bit("y"); break;
        case L: buttons_ |= bit("l"); break;
        case R: buttons_ |= bit("r"); break;
        case Start: buttons_ |= bit("start"); break;
        case Select: buttons_ |= bit("select"); break;
        default: break;
        }
    }
}

uint32_t TouchControls::buttons() const { return shown ? buttons_ : 0; }

std::optional<ImVec2> TouchControls::stylus() const {
    for (const auto &[id, f] : fingers_)
        if (f.control == Stylus) return f.at;
    return std::nullopt;
}

bool TouchControls::menu() {
    bool was = menu_;
    menu_ = false;
    return was;
}

void TouchControls::draw(ImDrawList *d, float w, float h, bool controls) const {
    const Layout l = layout(w, h);
    const ImU32 line = IM_COL32(255, 255, 255, 110), fill = IM_COL32(255, 255, 255, 34), held = IM_COL32(255, 255, 255, 110),
                text = IM_COL32(255, 255, 255, 190);
    const float thick = std::max(1.5f, 0.35f * l.u);
    auto label = [&](ImVec2 c, const char *s, float size) {
        ImFont *font = ImGui::GetFont();
        ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, s);
        d->AddText(font, size, {c.x - extent.x / 2, c.y - extent.y / 2}, text, s);
    };
    if (menu_button) {
        d->AddCircleFilled(l.menu, 4 * l.u, fill, 24);
        d->AddCircle(l.menu, 4 * l.u, line, 24, thick);
        for (int i = -1; i <= 1; i++)
            d->AddLine({l.menu.x - 2 * l.u, l.menu.y + i * 1.2f * l.u}, {l.menu.x + 2 * l.u, l.menu.y + i * 1.2f * l.u}, text, thick);
    }
    if (!shown || !controls) return;
    // the circle pad, its knob where the finger has it
    d->AddCircleFilled(l.circle, l.circle_r, fill, 48);
    d->AddCircle(l.circle, l.circle_r, line, 48, thick);
    d->AddCircleFilled({l.circle.x + circle_x_ * l.circle_r * 0.6f, l.circle.y - circle_y_ * l.circle_r * 0.6f}, l.circle_r * 0.45f,
                       circle_x_ || circle_y_ ? held : fill, 32);
    // the D-pad, a cross
    const float arm = l.pad_r, side = l.pad_r * 0.38f;
    auto arm_box = [&](float dx, float dy, uint32_t b) {
        ImVec2 c{l.pad.x + dx * arm * 0.62f, l.pad.y + dy * arm * 0.62f};
        ImVec2 half = dx ? ImVec2{arm * 0.4f, side} : ImVec2{side, arm * 0.4f};
        d->AddRectFilled({c.x - half.x, c.y - half.y}, {c.x + half.x, c.y + half.y}, (buttons_ & b) ? held : fill, 0.25f * side);
        d->AddRect({c.x - half.x, c.y - half.y}, {c.x + half.x, c.y + half.y}, line, 0.25f * side, 0, thick);
    };
    arm_box(0, -1, bit("up"));
    arm_box(0, 1, bit("down"));
    arm_box(-1, 0, bit("left"));
    arm_box(1, 0, bit("right"));
    // A, B, X, Y as the 3DS has them
    const ImVec2 faces[] = {{l.face.x + l.face_gap, l.face.y}, {l.face.x, l.face.y + l.face_gap}, {l.face.x, l.face.y - l.face_gap},
                            {l.face.x - l.face_gap, l.face.y}};
    const char *names[] = {"A", "B", "X", "Y"};
    const char *bits[] = {"a", "b", "x", "y"};
    for (int i = 0; i < 4; i++) {
        d->AddCircleFilled(faces[i], l.face_r, (buttons_ & bit(bits[i])) ? held : fill, 32);
        d->AddCircle(faces[i], l.face_r, line, 32, thick);
        label(faces[i], names[i], 6 * l.u);
    }
    // the shoulders, and Select and Start
    auto shoulder = [&](ImVec2 a, ImVec2 b, const char *name, uint32_t mask) {
        d->AddRectFilled(a, b, (buttons_ & mask) ? held : fill, 3 * l.u);
        d->AddRect(a, b, line, 3 * l.u, 0, thick);
        label({(a.x + b.x) / 2, (a.y + b.y) / 2}, name, 5 * l.u);
    };
    shoulder(l.l0, l.l1, "L", bit("l"));
    shoulder(l.r0, l.r1, "R", bit("r"));
    auto pill = [&](ImVec2 c, const char *name, uint32_t mask) {
        ImVec2 a{c.x - l.pill.x / 2, c.y - l.pill.y / 2}, b{c.x + l.pill.x / 2, c.y + l.pill.y / 2};
        d->AddRectFilled(a, b, (buttons_ & mask) ? held : fill, l.pill.y / 2);
        d->AddRect(a, b, line, l.pill.y / 2, 0, thick);
        label(c, name, 3.6f * l.u);
    };
    pill(l.select, "SELECT", bit("select"));
    pill(l.start, "START", bit("start"));
}

} // namespace frontend
