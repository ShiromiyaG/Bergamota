#include "battle_ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace frontend {

namespace {

// port_battle's layout (native/battle_ui.c): serial, mode, selected, count,
// then items of kind, x, y, the icon's four, and two texts of 24 UTF-16
constexpr uint32_t MAX_ITEMS = 16, TEXT = 24, ITEM_BYTES = 4 * 7 + 2 * TEXT * 2;

std::string utf8(const uint16_t *s) {
    std::string out;
    for (int i = 0; i < int(TEXT) && s[i]; i++) {
        uint32_t c = s[i];
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

// a label in white with a dark edge, as the games' are
void label(ImDrawList *d, ImFont *font, float size, ImVec2 at, const std::string &text, float s, ImU32 color = IM_COL32_WHITE) {
    (void)s;
    const float e = size * 0.075f; // the edge, as thick as the letters ask
    for (int k = 0; k < 8; k++) {
        float a = float(k) * 3.14159265f / 4;
        d->AddText(font, size, ImVec2(at.x + std::cos(a) * e, at.y + std::sin(a) * e), IM_COL32(24, 18, 40, 230), text.c_str());
    }
    d->AddText(font, size, at, color, text.c_str());
}

float text_width(ImFont *font, float size, const std::string &text) {
    return font->CalcTextSizeA(size, 1e9f, 0, text.c_str()).x;
}

} // namespace

BattleState read_battle(runtime::Memory &memory, uint32_t address) {
    BattleState state;
    uint32_t head[4];
    memory.read_bytes(address, head, sizeof head);
    state.serial = head[0], state.mode = head[1], state.selected = int(int32_t(head[2]));
    uint32_t count = std::min(head[3], MAX_ITEMS);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t raw[ITEM_BYTES];
        memory.read_bytes(address + 16 + i * ITEM_BYTES, raw, ITEM_BYTES);
        BattleItem item;
        std::memcpy(&item.kind, raw, 4);
        std::memcpy(&item.x, raw + 4, 4);
        std::memcpy(&item.y, raw + 8, 4);
        std::memcpy(item.icon, raw + 12, 16);
        uint16_t text[2][TEXT];
        std::memcpy(text, raw + 28, sizeof text);
        item.text[0] = utf8(text[0]), item.text[1] = utf8(text[1]);
        state.items.push_back(item);
    }
    return state;
}

std::vector<size_t> battle_order(const BattleState &state) {
    std::vector<size_t> order;
    auto take = [&](uint32_t kind) {
        for (size_t i = 0; i < state.items.size(); i++)
            if (state.items[i].kind == kind) order.push_back(i);
    };
    if (state.mode == 1) {
        for (uint32_t kind : {BATTLE_CARE, BATTLE_FIGHT, BATTLE_POKEMON, BATTLE_BAG, BATTLE_RUN}) take(kind);
    } else if (state.mode == 2) {
        for (uint32_t kind : {BATTLE_MOVE, BATTLE_ZMOVE, BATTLE_MEGA, BATTLE_ULTRA_BURST, BATTLE_BACK}) take(kind);
    }
    return order;
}

void BattleArt::load(const std::string &folder) {
    std::map<std::string, std::pair<int, float>> margins;
    if (FILE *f = std::fopen((folder + "/pieces.txt").c_str(), "r")) {
        char name[64];
        int margin;
        float scale;
        while (std::fscanf(f, "%63s %d %f", name, &margin, &scale) == 3) margins[name] = {margin, scale};
        std::fclose(f);
    }
    for (const auto &[name, m] : margins) {
        SDL_Surface *loaded = SDL_LoadPNG((folder + "/" + name + ".png").c_str());
        if (!loaded) continue;
        SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888); // bytes R, G, B, A
        SDL_DestroySurface(loaded);
        if (!rgba) continue;
        Piece piece;
        piece.width = rgba->w, piece.height = rgba->h, piece.margin = m.first, piece.scale = m.second;
        piece.rgba.resize(size_t(rgba->w) * rgba->h * 4);
        for (int y = 0; y < rgba->h; y++)
            std::memcpy(&piece.rgba[size_t(y) * rgba->w * 4], static_cast<uint8_t *>(rgba->pixels) + size_t(y) * rgba->pitch, size_t(rgba->w) * 4);
        SDL_DestroySurface(rgba);
        piece.rect = ImGui::GetIO().Fonts->AddCustomRectRegular(piece.width, piece.height);
        pieces[name] = std::move(piece);
    }
}

void BattleArt::finish() {
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    unsigned char *pixels = nullptr;
    int aw = 0, ah = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &aw, &ah);
    for (auto &[name, piece] : pieces) {
        const ImFontAtlasCustomRect *r = atlas->GetCustomRectByIndex(piece.rect);
        if (!r || !r->IsPacked()) continue;
        for (int y = 0; y < piece.height; y++)
            std::memcpy(pixels + (size_t(r->Y + y) * aw + r->X) * 4, &piece.rgba[size_t(y) * piece.width * 4], size_t(piece.width) * 4);
        atlas->CalcCustomRectUV(r, &piece.uv0, &piece.uv1);
        piece.rgba.clear();
    }
}

const BattleArt::Piece *BattleArt::find(const std::string &name) const {
    auto it = pieces.find(name);
    return it == pieces.end() || it->second.rect < 0 ? nullptr : &it->second;
}

namespace {

// a piece placed by its body: x, y its top left, in the window's points, s
// points a unit of the top screen's; tinted (white: as drawn)
void piece(ImDrawList *d, const BattleArt &art, const std::string &name, float x, float y, float s, ImU32 tint = IM_COL32_WHITE) {
    const BattleArt::Piece *p = art.find(name);
    if (!p) return;
    const float k = s / p->scale, m = float(p->margin) * k;
    d->AddImage(ImGui::GetIO().Fonts->TexID, ImVec2(x - m, y - m), ImVec2(x - m + float(p->width) * k, y - m + float(p->height) * k), p->uv0,
                p->uv1, tint);
}

const char *piece_name(uint32_t kind) {
    switch (kind) {
    case BATTLE_FIGHT: return "fight";
    case BATTLE_POKEMON: return "pokemon";
    case BATTLE_BAG: return "bag";
    case BATTLE_RUN: return "run";
    case BATTLE_CARE: return "care";
    case BATTLE_BACK: return "back";
    case BATTLE_ZMOVE: return "zmove";
    case BATTLE_MEGA: return "mega";
    case BATTLE_ULTRA_BURST: return "ultra";
    default: return "move";
    }
}

} // namespace

void draw_battle(ImDrawList *d, ImFont *font, const BattleArt &art, const BattleState &state, int chosen, float x0, float y0, float x1,
                 float y1, std::vector<BattleHit> &hits) {
    hits.clear();
    if (!state.mode) return;
    if (!font) font = ImGui::GetFont();
    // units of the top screen's height, 240 of them; laid out from the
    // bottom right corner, over the message box, as Sword and Shield has them
    const float s = (y1 - y0) / 240.0f, right = x1 - 8 * s, bottom = y1 - 48 * s;
    (void)x0;
    auto hit = [&](float x, float y, float w, float h, const BattleItem &item) { hits.push_back({x, y, x + w, y + h, item.x, item.y}); };
    std::vector<size_t> order = battle_order(state);
    if (state.mode == 1) {
        // Care small on top, then the commands; the chosen one out to the left
        const float w = 104 * s, h = 27 * s, gap = 3 * s;
        int big = 0;
        for (size_t i : order) big += state.items[i].kind != BATTLE_CARE;
        float y = bottom - float(big) * (h + gap) + gap;
        for (size_t i : order) {
            const BattleItem &item = state.items[i];
            const bool on = int(i) == chosen;
            if (item.kind == BATTLE_CARE) {
                const float cw = 54 * s, ch = 17 * s, cx = right - cw, cy = y - ch - 6 * s;
                piece(d, art, on ? "care_on" : "care", cx, cy, s);
                hit(cx, cy, cw, ch, item);
                continue;
            }
            float x = right - w - (on ? 8 * s : 0);
            piece(d, art, std::string(piece_name(item.kind)) + (on ? "_on" : ""), x, y, s);
            hit(x, y, w, h, item);
            y += h + gap;
        }
        return;
    }
    // the moves: their windows, each tinted by its type (not read yet: the
    // same blue for all), the name and the PP on them; back under them, the
    // Z-Move, Mega or Ultra Burst switch beside it
    const float w = 142 * s, h = 25 * s, gap = 3 * s, small_w = 54 * s, small_h = 17 * s;
    int moves = 0;
    for (size_t i : order) moves += state.items[i].kind == BATTLE_MOVE;
    float y = bottom - small_h - 6 * s - float(moves) * (h + gap) + gap;
    float x_small = right;
    for (size_t i : order) {
        const BattleItem &item = state.items[i];
        const bool on = int(i) == chosen;
        if (item.kind == BATTLE_MOVE) {
            float x = right - w - (on ? 8 * s : 0);
            piece(d, art, "move", x, y, s, IM_COL32(150, 172, 214, 255));
            piece(d, art, on ? "move_frame_on" : "move_frame", x, y, s);
            label(d, font, 12.5f * s, ImVec2(x + 14 * s, y + (h - 12.5f * s) / 2 - 0.5f * s), item.text[0], s);
            std::string pp = item.text[1].empty() ? "" : "PP " + item.text[1];
            float pw = text_width(font, 9.5f * s, pp);
            label(d, font, 9.5f * s, ImVec2(x + w - 12 * s - pw, y + (h - 9.5f * s) / 2), pp, s);
            hit(x, y, w, h, item);
            y += h + gap;
            continue;
        }
        const BattleArt::Piece *p = art.find(piece_name(item.kind));
        const float pw = p ? (float(p->width) / p->scale - 2 * float(p->margin) / p->scale) * s : small_w;
        x_small -= pw;
        piece(d, art, std::string(piece_name(item.kind)) + (on ? "_on" : ""), x_small, bottom - small_h, s);
        hit(x_small, bottom - small_h, pw, small_h, item);
        x_small -= 5 * s;
    }
}

bool BattleCursor::step(const BattleState &state, uint32_t &buttons, float &circle_y, float seconds, float &bx, float &by) {
    constexpr uint32_t A = 1, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128;
    std::vector<size_t> order = battle_order(state);
    if (!state.mode || order.empty()) {
        chosen = -1, mode_ = 0, touching_ = 0, held_ = 0;
        return false;
    }
    // a new list (the commands, the moves): from where the game's cursor is
    if (state.mode != mode_ || order.size() != count_ || chosen < 0 || size_t(chosen) >= state.items.size()) {
        mode_ = state.mode, count_ = order.size();
        bool found = std::find(order.begin(), order.end(), size_t(state.selected)) != order.end();
        chosen = int(found ? size_t(state.selected) : order[0]);
        // in the commands the game starts on Fight, after Care
        if (!found && state.mode == 1)
            for (size_t i : order)
                if (state.items[i].kind == BATTLE_FIGHT) chosen = int(i);
    }
    size_t at = size_t(std::find(order.begin(), order.end(), size_t(chosen)) - order.begin());
    if (at >= order.size()) at = 0;
    const int n = int(order.size());
    const int dir = (buttons & UP) || circle_y > 0.5f ? -1 : (buttons & DOWN) || circle_y < -0.5f ? 1 : 0;
    if (dir != held_) {
        held_ = dir, repeat_ = 0.35f;
        if (dir) at = size_t((int(at) + dir + n) % n);
    } else if (dir && (repeat_ -= seconds) <= 0) {
        repeat_ = 0.12f;
        at = size_t((int(at) + dir + n) % n);
    }
    chosen = int(order[at]);
    // A: a touch where the game's button is, a tenth of a second
    const bool a = buttons & A;
    if (a && !a_held_ && touching_ <= 0) {
        touching_ = 6;
        tx_ = state.items[size_t(chosen)].x, ty_ = state.items[size_t(chosen)].y;
    }
    a_held_ = a;
    buttons &= ~(A | UP | DOWN | LEFT | RIGHT);
    circle_y = 0;
    if (touching_ > 0) {
        touching_--;
        bx = tx_, by = ty_;
        return true;
    }
    return false;
}

void raster_ui(const ImDrawData *data, runtime::Picture &into) {
    unsigned char *atlas = nullptr;
    int aw = 0, ah = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    const int W = int(into.width), H = int(into.height);
    for (int l = 0; l < data->CmdListsCount; l++) {
        const ImDrawList *list = data->CmdLists[l];
        const ImDrawVert *v = list->VtxBuffer.Data;
        const ImDrawIdx *idx = list->IdxBuffer.Data;
        for (const ImDrawCmd &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int cx0 = std::max(0, int(cmd.ClipRect.x)), cy0 = std::max(0, int(cmd.ClipRect.y));
            const int cx1 = std::min(W, int(std::ceil(cmd.ClipRect.z))), cy1 = std::min(H, int(std::ceil(cmd.ClipRect.w)));
            for (unsigned t = 0; t + 2 < cmd.ElemCount + 0; t += 3) {
                const ImDrawVert &a = v[cmd.VtxOffset + idx[cmd.IdxOffset + t]], &b = v[cmd.VtxOffset + idx[cmd.IdxOffset + t + 1]],
                                 &c = v[cmd.VtxOffset + idx[cmd.IdxOffset + t + 2]];
                float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
                if (std::abs(area) < 1e-6f) continue;
                int bx0 = std::max(cx0, int(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))));
                int by0 = std::max(cy0, int(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))));
                int bx1 = std::min(cx1, int(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))));
                int by1 = std::min(cy1, int(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))));
                for (int y = by0; y < by1; y++) {
                    for (int x = bx0; x < bx1; x++) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
                        float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
                        float w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float u = a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2, vv = a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2;
                        int tx = std::clamp(int(u * aw), 0, aw - 1), ty = std::clamp(int(vv * ah), 0, ah - 1);
                        const unsigned char *texel = atlas + (size_t(ty) * aw + tx) * 4;
                        float col[4];
                        for (int k = 0; k < 4; k++) {
                            float ca = float(a.col >> (8 * k) & 0xFF), cb = float(b.col >> (8 * k) & 0xFF), cc = float(c.col >> (8 * k) & 0xFF);
                            col[k] = (ca * w0 + cb * w1 + cc * w2) / 255.0f * (texel[k] / 255.0f);
                        }
                        uint8_t *out = &into.rgba[(size_t(y) * W + x) * 4];
                        for (int k = 0; k < 3; k++) out[k] = uint8_t(std::clamp(out[k] * (1 - col[3]) + col[k] * 255.0f * col[3], 0.0f, 255.0f));
                    }
                }
            }
        }
    }
}

} // namespace frontend
