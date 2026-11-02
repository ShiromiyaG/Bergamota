// A port's battle UI on the top screen, for the single screen: Sword and
// Shield's layout (the commands, then the moves, stacked on the right), in a
// port's own art (PNG pieces, Solstice's ui/battle/, drawn in the game's
// manner), from what the port's code reads off the game's bottom screen
// (Solstice's native/battle_ui.c, port_battle). It has a cursor of its own,
// moved up and down in its own order: A, or a click, touches the bottom
// screen where the game's button for the item is. The player draws it over
// the window; boot rasterizes it into its composed shots (raster_ui) to look
// at without playing.
#ifndef FRONTEND_BATTLE_UI_H
#define FRONTEND_BATTLE_UI_H

#include "runtime/gpu.h"
#include "runtime/memory.h"

#include <imgui.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace frontend {

enum BattleKind : uint32_t {
    BATTLE_FIGHT = 1, BATTLE_POKEMON, BATTLE_BAG, BATTLE_RUN, BATTLE_CARE, BATTLE_BACK, BATTLE_MOVE, BATTLE_ZMOVE,
    BATTLE_MEGA, BATTLE_ULTRA_BURST, BATTLE_BALL, BATTLE_INFO
};

struct BattleItem {
    uint32_t kind = 0;
    float x = 0, y = 0;   // where to touch it, bottom screen pixels
    float icon[4] = {};   // a move's type icon on the bottom screen: x, y, width, height
    std::string text[2];  // a move's name and PP
};

struct BattleState {
    uint32_t serial = 0, mode = 0; // mode 1 the commands, 2 the moves
    int selected = -1;             // the game's own cursor
    std::vector<BattleItem> items;
};

// port_battle at address, as the port's code left it
BattleState read_battle(runtime::Memory &memory, uint32_t address);

// the items the UI shows, in its order, top to bottom (indices of items)
std::vector<size_t> battle_order(const BattleState &state);

// the art: each piece a picture packed into Dear ImGui's font atlas (drawn
// as its letters are, in the player and in boot's rasterizer), placed by its
// body (the picture less its margin for the chosen one's glow; pieces.txt
// gives each its margin and its pixels a unit of the top screen's 240)
struct BattleArt {
    struct Piece {
        int rect = -1;
        int width = 0, height = 0, margin = 0;
        float scale = 4;
        std::vector<uint8_t> rgba;
        ImVec2 uv0, uv1;
    };
    std::map<std::string, Piece> pieces;
    // the pieces in folder added to the atlas, before it is built
    void load(const std::string &folder);
    // their pixels written into the built atlas
    void finish();
    const Piece *find(const std::string &name) const;
};

// where a click touches: a box of the window and the bottom screen's point
struct BattleHit {
    float x0, y0, x1, y1;
    float bx, by;
};

// the UI drawn over the top screen, shown at x0,y0-x1,y1 of the window;
// chosen, the item (an index of items) its cursor is on; the boxes a click
// touches in hits. font: the UI's (Nunito in Solstice's package), null for
// ImGui's own
void draw_battle(ImDrawList *draw, ImFont *font, const BattleArt &art, const BattleState &state, int chosen, float x0,
                 float y0, float x1, float y1, std::vector<BattleHit> &hits);

// the UI's own cursor: up and down move it in the UI's order (held, it
// repeats), A touches the item it is on; the game's cursor is left alone,
// and gets neither. Starts where the game's is when the UI comes up
struct BattleCursor {
    int chosen = -1; // an index of the state's items
    // the input of this frame: buttons (the 3DS's bits) and the circle pad's
    // y, the ones the UI takes taken out; a touch to make, if any, in bx, by
    bool step(const BattleState &state, uint32_t &buttons, float &circle_y, float seconds, float &bx, float &by);

private:
    uint32_t mode_ = 0, serial_items_ = 0;
    size_t count_ = 0;
    int held_ = 0;          // -1 up, 1 down
    float repeat_ = 0;
    bool a_held_ = false;
    int touching_ = 0;      // frames of a touch left
    float tx_ = 0, ty_ = 0;
};

// a frame of Dear ImGui's draw lists drawn into a picture (boot's shots):
// triangles, their vertex colors and the atlas, blended over it
void raster_ui(const ImDrawData *data, runtime::Picture &into);

} // namespace frontend

#endif
