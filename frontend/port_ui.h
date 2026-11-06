// What a port adds to the player and to boot on the host's side: a UI of its
// own over the game's screens, fed by what the port's code (its native/, the
// symbols in decomp.txt) leaves in the game's memory. The runtime has none
// (port_ui_none.cpp); a port builds its own in with RUNTIME_PORT_UI=<folder>
// (CMakeLists.txt), a folder whose CMakeLists.txt makes the port_ui library
// and defines make_port_ui (Solstice's frontend/, its battle UI).
//
// blank runs on the game's thread, the rest on the window's, in this order
// each frame: frame, click and input (the frame's input), draw, then up (the
// layout of the next frame).
#ifndef FRONTEND_PORT_UI_H
#define FRONTEND_PORT_UI_H

#include "runtime/decomp.h"
#include "runtime/memory.h"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace frontend {

struct PortUi {
    // where the top screen is shown, in the window's points
    struct Box {
        float x0, y0, x1, y1;
    };

    virtual ~PortUi() = default;
    // its pictures into Dear ImGui's atlas, which it builds then. font: the
    // UI's (game.ui_font), null for ImGui's own; art: game.ui_art, empty for
    // the port's own folder beside the player (base)
    virtual void start(ImFont *font, const std::filesystem::path &base, const std::string &art) = 0;
    // once a vertical blank, on the game's thread: what the port's code left
    virtual void blank(runtime::Memory &memory, uint64_t vblanks) = 0;
    // the last blank's reading, taken for this frame
    virtual void frame() = 0;
    // a click at x, y (the window's points) on what draw drew last: true,
    // with where it touches the bottom screen (its pixels) in bx, by
    virtual bool click(float x, float y, float &bx, float &by) const = 0;
    // the frame's input while the UI is up (up false: let go): what it takes
    // taken out of buttons (the 3DS's bits) and circle_y; true with a touch
    // to make at bx, by
    virtual bool input(bool up, uint32_t &buttons, float &circle_y, float seconds, float &bx, float &by) = 0;
    // drawn over the top screen at each of tops; none: nothing drawn, and
    // nothing to click
    virtual void draw(ImDrawList *draw, const std::vector<Box> &tops) = 0;
    // whether it is up, with the scene's layout ("panel"...); up, the single
    // screen shows the top screen alone, the UI over it
    virtual bool up(const std::string &layout) const = 0;
};

// the port's UI for its code, null when the code has none of it
std::unique_ptr<PortUi> make_port_ui(const runtime::Decomp &decomp);

} // namespace frontend

#endif
