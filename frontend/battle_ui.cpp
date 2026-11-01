#include "battle_ui.h"

#include <algorithm>
#include <cmath>
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

ImU32 rgb(uint32_t hex, int alpha = 255) { return IM_COL32(hex >> 16 & 0xFF, hex >> 8 & 0xFF, hex & 0xFF, alpha); }

// the commands' colors, as Ultra Sun and Ultra Moon's buttons have them: a
// gradient from the first to the second, corner to corner
struct Look {
    uint32_t from, to;
    const char *label;
};

const Look &look_of(uint32_t kind) {
    static const Look FIGHT{0xD8263F, 0xFF7A45, "FIGHT"}, POKEMON{0x1E9E57, 0x8BD448, "POK\xC3\x89MON"},
        BAG{0xE08A10, 0xFFD23C, "BAG"}, RUN{0x2F63C8, 0x5FC6F4, "RUN"}, CARE{0xD84C8E, 0xFF9CC4, "Care"},
        BACK{0x4E5568, 0x8C95AB, "Back"}, ZMOVE{0xC79A12, 0xFFE15A, "Z-Move"}, MEGA{0x6A3FC0, 0xC08CFF, "Mega"},
        ULTRA{0xC9C9C9, 0xFFFFFF, "Ultra Burst"}, MOVE{0x2B3B5E, 0x4F6A9E, ""};
    switch (kind) {
    case BATTLE_FIGHT: return FIGHT;
    case BATTLE_POKEMON: return POKEMON;
    case BATTLE_BAG: return BAG;
    case BATTLE_RUN: return RUN;
    case BATTLE_CARE: return CARE;
    case BATTLE_BACK: return BACK;
    case BATTLE_ZMOVE: return ZMOVE;
    case BATTLE_MEGA: return MEGA;
    case BATTLE_ULTRA_BURST: return ULTRA;
    default: return MOVE;
    }
}

ImU32 mix(uint32_t a, uint32_t b, float t, int alpha) {
    auto ch = [&](int s) { return int(std::lround((a >> s & 0xFF) * (1 - t) + (b >> s & 0xFF) * t)); };
    return IM_COL32(ch(16), ch(8), ch(0), alpha);
}

// a bar in the games' way: slanted ends, a dark outline, a light rim inside
// and a gradient corner to corner; selected, a white rim and brighter
void bar(ImDrawList *d, float x, float y, float w, float h, float slant, const Look &look, bool selected, float s) {
    const int alpha = 255;
    ImVec2 outer[4] = {{x + slant, y}, {x + w, y}, {x + w - slant, y + h}, {x, y + h}};
    // the outline, a little larger
    const float o = 1.6f * s;
    ImVec2 rim[4] = {{outer[0].x - o * 0.5f, outer[0].y - o}, {outer[1].x + o, outer[1].y - o},
                     {outer[2].x + o * 0.5f, outer[2].y + o}, {outer[3].x - o, outer[3].y + o}};
    d->AddConvexPolyFilled(rim, 4, IM_COL32(24, 18, 40, 235));
    // the body, its corners' colors a gradient corner to corner
    uint32_t from = look.from, to = look.to;
    if (selected) {
        auto lift = [](uint32_t c) {
            int r = std::min(255, int(c >> 16 & 0xFF) + 40), g = std::min(255, int(c >> 8 & 0xFF) + 40), b = std::min(255, int(c & 0xFF) + 40);
            return uint32_t(r << 16 | g << 8 | b);
        };
        from = lift(from), to = lift(to);
    }
    ImU32 colors[4] = {mix(from, to, 0.0f, alpha), mix(from, to, 0.55f, alpha), mix(from, to, 1.0f, alpha), mix(from, to, 0.45f, alpha)};
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve(6, 4);
    ImDrawIdx base = ImDrawIdx(d->_VtxCurrentIdx);
    for (int i = 0; i < 4; i++) d->PrimWriteVtx(outer[i], white, colors[i]);
    d->PrimWriteIdx(base), d->PrimWriteIdx(ImDrawIdx(base + 1)), d->PrimWriteIdx(ImDrawIdx(base + 2));
    d->PrimWriteIdx(base), d->PrimWriteIdx(ImDrawIdx(base + 2)), d->PrimWriteIdx(ImDrawIdx(base + 3));
    // a sheen across the top half
    ImVec2 sheen[4] = {outer[0], outer[1], {x + w - slant * 0.5f, y + h * 0.45f}, {x + slant * 0.5f, y + h * 0.45f}};
    d->AddConvexPolyFilled(sheen, 4, IM_COL32(255, 255, 255, 38));
    // the rim
    const float in = 1.4f * s;
    ImVec2 inner[4] = {{outer[0].x + in, outer[0].y + in}, {outer[1].x - in, outer[1].y + in},
                       {outer[2].x - in, outer[2].y - in}, {outer[3].x + in, outer[3].y - in}};
    d->AddPolyline(inner, 4, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 110), ImDrawFlags_Closed,
                   (selected ? 1.8f : 1.0f) * s);
}

// a label in white with a dark edge, as the games' are
void label(ImDrawList *d, ImFont *font, float size, ImVec2 at, const std::string &text, float s, ImU32 color = IM_COL32_WHITE) {
    const float e = 1.1f * s;
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

void draw_battle(ImDrawList *d, ImFont *font, const BattleState &state, float x0, float y0, float x1, float y1,
                 std::vector<BattleHit> &hits) {
    hits.clear();
    if (!state.mode) return;
    if (!font) font = ImGui::GetFont();
    // units of the top screen's height, 240 of them; laid out from the
    // bottom right corner, as Sword and Shield has them
    const float s = (y1 - y0) / 240.0f, right = x1 - 8 * s, bottom = y1 - 50 * s; // over the message box
    auto add_hit = [&](float x, float y, float w, float h, const BattleItem &item) {
        hits.push_back({x, y, x + w, y + h, item.x, item.y});
    };
    const BattleItem *chosen = state.selected >= 0 && size_t(state.selected) < state.items.size() ? &state.items[state.selected] : nullptr;
    if (state.mode == 1) {
        // the commands, Fight on top, Run at the bottom; the chosen one out
        // to the left
        const float w = 96 * s, h = 19 * s, gap = 4 * s;
        const uint32_t order[] = {BATTLE_FIGHT, BATTLE_POKEMON, BATTLE_BAG, BATTLE_RUN};
        float y = bottom - 4 * h - 3 * gap;
        for (uint32_t kind : order) {
            auto it = std::find_if(state.items.begin(), state.items.end(), [&](const BattleItem &i) { return i.kind == kind; });
            if (it != state.items.end()) {
                bool selected = chosen == &*it;
                float x = right - w - (selected ? 10 * s : 0);
                const Look &look = look_of(kind);
                bar(d, x, y, w, h, 7 * s, look, selected, s);
                label(d, font, 12 * s, ImVec2(x + 15 * s, y + (h - 12 * s) / 2), look.label, s);
                add_hit(x, y, w, h, *it);
            }
            y += h + gap;
        }
        for (const BattleItem &item : state.items) {
            if (item.kind != BATTLE_CARE) continue;
            const float cw = 54 * s, ch = 15 * s, cx = right - cw, cy = bottom - 4 * h - 3 * gap - ch - 8 * s;
            bar(d, cx, cy, cw, ch, 6 * s, look_of(BATTLE_CARE), false, s);
            label(d, font, 10 * s, ImVec2(cx + 12 * s, cy + (ch - 10 * s) / 2), look_of(BATTLE_CARE).label, s);
            add_hit(cx, cy, cw, ch, item);
        }
        return;
    }
    // the moves: four bars, the name and the PP; back under them, the
    // Z-Move, Mega or Ultra Burst switch beside it
    const float w = 140 * s, h = 23 * s, gap = 4 * s, small = 15 * s;
    std::vector<const BattleItem *> moves;
    for (const BattleItem &item : state.items)
        if (item.kind == BATTLE_MOVE) moves.push_back(&item);
    float y = bottom - small - 8 * s - float(moves.size()) * (h + gap) + gap;
    for (const BattleItem *move : moves) {
        bool selected = chosen == move;
        float x = right - w - (selected ? 10 * s : 0);
        bar(d, x, y, w, h, 8 * s, look_of(BATTLE_MOVE), selected, s);
        label(d, font, 12 * s, ImVec2(x + 14 * s, y + (h - 12 * s) / 2), move->text[0], s);
        std::string pp = move->text[1].empty() ? "" : "PP " + move->text[1];
        float pw = text_width(font, 9 * s, pp);
        label(d, font, 9 * s, ImVec2(x + w - 12 * s - pw, y + (h - 9 * s) / 2 + 1 * s), pp, s, IM_COL32(230, 236, 255, 255));
        add_hit(x, y, w, h, *move);
        y += h + gap;
    }
    float x = right;
    for (const BattleItem &item : state.items) {
        if (item.kind != BATTLE_BACK) continue;
        const float bw = 46 * s;
        x -= bw;
        bar(d, x, bottom - small, bw, small, 6 * s, look_of(BATTLE_BACK), chosen == &item, s);
        label(d, font, 10 * s, ImVec2(x + 12 * s, bottom - small + (small - 10 * s) / 2), look_of(BATTLE_BACK).label, s);
        add_hit(x, bottom - small, bw, small, item);
        x -= 6 * s;
    }
    for (const BattleItem &item : state.items) {
        if (item.kind != BATTLE_ZMOVE && item.kind != BATTLE_MEGA && item.kind != BATTLE_ULTRA_BURST) continue;
        const Look &look = look_of(item.kind);
        const float zw = text_width(font, 10 * s, look.label) + 26 * s;
        x -= zw;
        bar(d, x, bottom - small, zw, small, 6 * s, look, chosen == &item, s);
        label(d, font, 10 * s, ImVec2(x + 12 * s, bottom - small + (small - 10 * s) / 2), look.label, s);
        add_hit(x, bottom - small, zw, small, item);
        x -= 6 * s;
    }
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
