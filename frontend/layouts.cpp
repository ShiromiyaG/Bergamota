#include "layouts.h"

#include <algorithm>
#include <cmath>

namespace frontend {

using runtime::Picture;
using runtime::VulkanPresenter;
using Placement = VulkanPresenter::Placement;

void place_single(VulkanPresenter::View &view, float shown, float size, const std::string &corner, float lift) {
    view.canvas[0] = view.top, view.canvas[1] = 240;
    Placement top;
    top.to[2] = view.top, top.to[3] = 240;
    view.placements = {top};
    if (shown <= 0) return;
    const float margin = 6, w = 320 * size, h = 240 * size;
    const bool left = corner.find("left") != std::string::npos;
    const bool high = corner.rfind("top", 0) == 0;
    // eased, in from its side
    float t = shown * shown * (3 - 2 * shown), away = (1 - t) * (w + margin);
    Placement panel;
    panel.screen = 1;
    panel.to[0] = left ? margin - away : view.top - margin - w + away;
    panel.to[1] = high ? margin : std::max(margin, 240 - margin - h - lift);
    panel.to[2] = panel.to[0] + w;
    panel.to[3] = panel.to[1] + h;
    view.placements.push_back(panel);
}

void place_stacked(VulkanPresenter::View &view) {
    float shape = view.height ? float(view.width) / float(view.height) : 1.0f;
    float w = std::max(view.top, 480 * shape), h = 480;
    view.canvas[0] = w, view.canvas[1] = h;
    // the fill covers the canvas, the top screen's shape kept
    float fw = std::max(w, h * view.top / 240), fh = fw * 240 / view.top;
    Placement fill;
    fill.to[0] = (w - fw) / 2, fill.to[1] = (h - fh) / 2, fill.to[2] = fill.to[0] + fw, fill.to[3] = fill.to[1] + fh;
    fill.brightness = 0.4f;
    fill.blur = 6;
    Placement top;
    top.to[0] = (w - view.top) / 2, top.to[2] = top.to[0] + view.top, top.to[3] = 240;
    Placement bottom;
    bottom.screen = 1;
    bottom.to[0] = (w - 320) / 2, bottom.to[1] = 240, bottom.to[2] = bottom.to[0] + 320, bottom.to[3] = 480;
    view.placements = {fill, top, bottom};
}

void place(VulkanPresenter::View &view, const std::string &layout, float shown, float size, const std::string &corner,
           float lift) {
    if (layout == "stacked") place_stacked(view);
    else place_single(view, layout == "top" ? 0.0f : shown, size, corner, lift);
}

void swap_screens(VulkanPresenter::View &view) {
    VulkanPresenter::place_screens(view);
    const float widths[2] = {view.top, 320};
    for (Placement &p : view.placements) {
        const float scale = widths[p.screen ? 0 : 1] / widths[p.screen ? 1 : 0];
        const float w = p.to[2] - p.to[0], grown = w * scale, middle = (p.to[0] + p.to[2]) / 2;
        if (w >= view.canvas[0] * 0.9f || std::abs(middle - view.canvas[0] / 2) < 1) p.to[0] = middle - grown / 2;
        else if (middle > view.canvas[0] / 2) p.to[0] = p.to[2] - grown;
        p.to[2] = p.to[0] + grown;
        p.screen ^= 1;
    }
}

void in_window(VulkanPresenter::View &view) {
    VulkanPresenter::place_screens(view);
    float scale, x0, y0;
    VulkanPresenter::fit(view, scale, x0, y0);
    for (Placement &p : view.placements) {
        p.to[0] = x0 + p.to[0] * scale, p.to[1] = y0 + p.to[1] * scale;
        p.to[2] = x0 + p.to[2] * scale, p.to[3] = y0 + p.to[3] * scale;
    }
    view.canvas[0] = float(view.width), view.canvas[1] = float(view.height);
}

// a box blur of radius pixels each way, across then down
static Picture blurred(const Picture &p, int radius) {
    Picture out = p, across = p;
    const int w = int(p.width), h = int(p.height);
    for (int pass = 0; pass < 2; pass++) {
        const Picture &from = pass ? across : p;
        Picture &to = pass ? out : across;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int sum[3] = {}, n = 0;
                for (int d = -radius; d <= radius; d++) {
                    int sx = pass ? x : std::clamp(x + d, 0, w - 1), sy = pass ? std::clamp(y + d, 0, h - 1) : y;
                    const uint8_t *s = &from.rgba[(size_t(sy) * w + sx) * 4];
                    for (int c = 0; c < 3; c++) sum[c] += s[c];
                    n++;
                }
                uint8_t *d = &to.rgba[(size_t(y) * w + x) * 4];
                for (int c = 0; c < 3; c++) d[c] = uint8_t(sum[c] / n);
            }
        }
    }
    return out;
}

Picture compose(VulkanPresenter::View view, const Picture &top, const Picture &bottom) {
    VulkanPresenter::place_screens(view);
    Picture out;
    out.width = uint32_t(std::lround(view.canvas[0])), out.height = uint32_t(std::lround(view.canvas[1]));
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    for (const Placement &p : view.placements) {
        const Picture &screen = p.screen ? bottom : top;
        if (screen.rgba.empty() || p.opacity <= 0 || p.to[2] <= p.to[0] || p.to[3] <= p.to[1]) continue;
        const Picture source = p.blur > 0 ? blurred(screen, std::max(1, int(std::lround(p.blur)))) : screen;
        const int x0 = std::max(0, int(std::floor(p.to[0]))), x1 = std::min(int(out.width), int(std::ceil(p.to[2])));
        const int y0 = std::max(0, int(std::floor(p.to[1]))), y1 = std::min(int(out.height), int(std::ceil(p.to[3])));
        const float alpha = std::min(p.opacity, 1.0f);
        for (int y = y0; y < y1; y++) {
            float v = (y + 0.5f - p.to[1]) / (p.to[3] - p.to[1]);
            if (v < 0 || v >= 1) continue;
            int sy = std::clamp(int((p.from[1] + v * (p.from[3] - p.from[1])) * source.height), 0, int(source.height) - 1);
            for (int x = x0; x < x1; x++) {
                float u = (x + 0.5f - p.to[0]) / (p.to[2] - p.to[0]);
                if (u < 0 || u >= 1) continue;
                int sx = std::clamp(int((p.from[0] + u * (p.from[2] - p.from[0])) * source.width), 0, int(source.width) - 1);
                const uint8_t *s = &source.rgba[(size_t(sy) * source.width + sx) * 4];
                uint8_t *d = &out.rgba[(size_t(y) * out.width + x) * 4];
                for (int c = 0; c < 3; c++) {
                    float shown = std::min(s[c] * p.brightness, 255.0f);
                    d[c] = uint8_t(std::lround(d[c] * (1 - alpha) + shown * alpha));
                }
            }
        }
    }
    return out;
}

} // namespace frontend
