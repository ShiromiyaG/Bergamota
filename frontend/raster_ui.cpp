#include "raster_ui.h"

#include <algorithm>
#include <cmath>

namespace frontend {

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
