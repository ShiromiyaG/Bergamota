// A port's battle UI on the top screen, for the single screen: Sword and
// Shield's layout (the commands, then the moves, stacked on the right), drawn
// with Dear ImGui's draw lists in shapes of our own, from what the port's
// code reads off the game's bottom screen (Solstice's native/battle_ui.c,
// port_battle). Nothing is the game's art; a click on an item touches the
// bottom screen where the game's button for it is. The player draws it over
// the window; boot rasterizes it into its composed shots (raster_ui) to look
// at without playing.
#ifndef FRONTEND_BATTLE_UI_H
#define FRONTEND_BATTLE_UI_H

#include "runtime/gpu.h"
#include "runtime/memory.h"

#include <imgui.h>

#include <cstdint>
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
    int selected = -1;
    std::vector<BattleItem> items;
};

// port_battle at address, as the port's code left it
BattleState read_battle(runtime::Memory &memory, uint32_t address);

// where a click touches: a box of the window and the bottom screen's point
struct BattleHit {
    float x0, y0, x1, y1;
    float bx, by;
};

// the UI drawn over the top screen, shown at x0,y0-x1,y1 of the window (its
// 2D kept 5:3 in the middle when wider); the boxes a click touches in hits.
// font: the UI's (Nunito in Solstice's package), null for ImGui's own
void draw_battle(ImDrawList *draw, ImFont *font, const BattleState &state, float x0, float y0, float x1, float y1,
                 std::vector<BattleHit> &hits);

// a frame of Dear ImGui's draw lists drawn into a picture (boot's shots):
// triangles, their vertex colors and the font's texture, blended over it
void raster_ui(const ImDrawData *data, runtime::Picture &into);

} // namespace frontend

#endif
