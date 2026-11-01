// Frames drawn between the game's: each frame's draws kept and drawn again
// with every vertex halfway between the frame before and this one. (impl.h)

#include "impl.h"

namespace runtime {

using Impl = VulkanRenderer::Impl;

void Impl::note_draw(Pica &p, const VkImageView *views) {
    last_pica = &p;
    if (tables_kept_version != p.tables_version) {
        tables_kept = std::make_shared<const Tables>(p.light_tables);
        tables_kept_version = p.tables_version;
    }
    // the six combiner stages without their constant colors (fades move
    // those), and the textures
    static constexpr uint32_t STAGES[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
    uint64_t look[13] = {};
    for (int i = 0; i < 6; i++) {
        const uint32_t *r = &p.regs[STAGES[i]];
        look[i * 2] = r[0] | uint64_t(r[1]) << 32;
        look[i * 2 + 1] = r[2] | uint64_t(r[4]) << 32;
    }
    look[12] = p.regs[COLOR_OPERATION];
    Op op{Op::Draw, std::vector<uint32_t>(p.regs.begin(), p.regs.end()), triangles, {views[0], views[1], views[2]},
          tables_kept, p.tables_version, {}, {}};
    op.look = hash_bytes(reinterpret_cast<const uint8_t *>(look), sizeof look) ^
              hash_bytes(reinterpret_cast<const uint8_t *>(views), 3 * sizeof(VkImageView)) * 3;
    // the first only: the others are often projected from the camera
    // (shadows, reflections) and change as it moves
    std::vector<float> uvs;
    uvs.reserve(triangles.size() * 2);
    for (const Vertex &v : triangles) uvs.insert(uvs.end(), {v.uv0[0], v.uv0[1]});
    op.uvs = hash_bytes(reinterpret_cast<const uint8_t *>(uvs.data()), uvs.size() * sizeof(float));
    ops_now.push_back(std::move(op));
}

uint32_t Impl::op_target(const Op &op) { return (op.regs[COLOR_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3; }

const Impl::Op *Impl::match(const std::vector<Op> &list, Matching &m, const Op &op) {
    if (m.used.size() != list.size()) m.used.assign(list.size(), false);
    int same = -1;
    float least = 0.5f; // past it, a jump
    for (size_t i = 0; i < list.size(); i++) {
        const Op &before = list[i];
        if (m.used[i] || before.uvs != op.uvs || before.vertices.size() != op.vertices.size() || before.look != op.look) continue;
        if (float d = moved(before, op); d < least) least = d, same = int(i);
    }
    int best = same;
    if (best < 0) {
        size_t start = m.from > 8 ? m.from - 8 : 0, end = std::min(list.size(), m.from + 48);
        float nearest = 0.25f; // an eighth of the screen
        int in_order = -1;
        for (size_t i = start; i < end; i++) {
            const Op &before = list[i];
            if (m.used[i] || before.vertices.size() != op.vertices.size() || before.look != op.look) continue;
            float d = moved(before, op);
            if (d >= 0.25f) continue;
            if (in_order < 0 && i >= m.from) in_order = int(i);
            if (d < nearest) nearest = d, best = int(i);
        }
        if (in_order >= 0) best = in_order;
    }
    if (best < 0) return nullptr;
    m.used[best] = true;
    m.from = size_t(best) + 1;
    return &list[best];
}

// the most a vertex moved, in halves of the screen. only the vertices
// well in front of the eye and on the screen or near it count: near the
// eye, or far off the screen, a small move projects to a huge one (the
// ground reaching under the camera, a big piece mostly off the screen)
float Impl::moved(const Op &before, const Op &now) {
    float far = 0;
    for (size_t i = 0; i < now.vertices.size(); i++) {
        far = std::max({far, before.vertices[i].position[3], now.vertices[i].position[3]});
    }
    const float near = far * 0.05f;
    float most = 0;
    for (size_t i = 0; i < now.vertices.size(); i++) {
        const Vec4 &a = before.vertices[i].position, &b = now.vertices[i].position;
        if (a[3] <= near || b[3] <= near) continue;
        float ax = a[0] / a[3], ay = a[1] / a[3], bx = b[0] / b[3], by = b[1] / b[3];
        bool on_screen = (std::abs(ax) <= 1.5f && std::abs(ay) <= 1.5f) || (std::abs(bx) <= 1.5f && std::abs(by) <= 1.5f);
        if (!on_screen) continue;
        most = std::max({most, std::abs(ax - bx), std::abs(ay - by)});
    }
    return most;
}

// a pair that jumps (more than a quarter of the screen) is not the same
// thing moving (a camera cut): drawn where it is now
bool Impl::jumps(const Op &before, const Op &now) { return moved(before, now) > 0.5f; }

// the game frame just drawn, drawn again at t (0 the frame before, 1
// this one) into the between images
void Impl::draw_between(float t) {
    Pica &p = *last_pica;
    end_rendering();
    // each buffer's between image starts from what the game's holds, its
    // depth and stencil too
    struct Kept {
        bool dirty, stale;
        uint64_t version, converted;
    };
    std::vector<Kept> kept;
    between_views.clear();
    // a depth buffer read as color (an effect reading the depth): made
    // again from the between depth when a replayed draw reads it
    std::unordered_map<VkImageView, Surface *> depth_views;
    std::vector<Surface *> depth_redone;
    for (auto &s : surfaces) {
        if (s->as_color.image) depth_views[s->as_color.view] = s.get();
    }
    for (auto &s : surfaces) {
        kept.push_back({s->dirty, s->stale, s->version, s->converted});
        if (!s->between.image) s->between = image(s->width, s->height, s->depth ? depth_format : VK_FORMAT_R8G8B8A8_UNORM, s->depth);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {
            VkImageAspectFlags(s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        c.extent = {s->width, s->height, 1};
        vkCmdCopyImage(cmd, s->image.image, VK_IMAGE_LAYOUT_GENERAL, s->between.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        between_views[s->image.view] = s->between.view;
        std::swap(s->image, s->between);
    }
    barrier();
    auto regs = p.regs;
    auto tables = p.light_tables;
    uint64_t tables_version = p.tables_version;
    replaying = true;
    std::map<uint32_t, Matching> matched;
    for (Op &op : ops_now) {
        if (op.kind == Op::Fill) {
            fill(op.fill[0], op.fill[1], op.fill[2], op.fill[3]);
            continue;
        }
        if (op.kind == Op::Transfer) {
            transfer(op.transfer);
            continue;
        }
        const Op *before = nullptr;
        auto last = draws_before.find(op_target(op));
        if (last != draws_before.end()) before = match(last->second, matched[op_target(op)], op);
        triangles = op.vertices;
        if (before && !jumps(*before, op)) {
            for (size_t i = 0; i < triangles.size(); i++) {
                const Vec4 &a = before->vertices[i].position, &b = op.vertices[i].position;
                for (int k = 0; k < 4; k++) triangles[i].position[k] = a[k] + (b[k] - a[k]) * t;
            }
        }
        std::copy(op.regs.begin(), op.regs.end(), p.regs.begin());
        if (op.tables) p.light_tables = *op.tables;
        p.tables_version = op.tables_version;
        VkImageView views[3];
        for (int u = 0; u < 3; u++) {
            auto it = between_views.find(op.views[u]);
            views[u] = it != between_views.end() ? it->second : op.views[u];
            if (auto d = depth_views.find(op.views[u]); d != depth_views.end()) {
                Surface &s = *d->second;
                if (std::find(depth_redone.begin(), depth_redone.end(), &s) == depth_redone.end()) {
                    depth_redone.push_back(&s);
                    s.converted = ~0ull;
                }
                views[u] = depth_as_color(s);
            }
        }
        forced_views = views;
        render(p);
    }
    end_rendering();
    forced_views = nullptr;
    replaying = false;
    p.regs = regs;
    p.light_tables = tables;
    p.tables_version = tables_version;
    for (size_t i = 0; i < surfaces.size(); i++) {
        Surface &s = *surfaces[i];
        std::swap(s.image, s.between);
        s.dirty = kept[i].dirty, s.stale = kept[i].stale, s.version = kept[i].version, s.converted = kept[i].converted;
        // its color copy holds the between depth now: made again when the game reads it
        if (std::find(depth_redone.begin(), depth_redone.end(), &s) != depth_redone.end()) s.converted = ~0ull;
    }
    barrier();
}

// a frame for the top screen ended in a transfer into output: drawn
// between when it came two blanks or more after the one before (a scene
// at 60 needs none), then kept as the frame before
void Impl::frame_end(uint32_t output) {
    top_frames++;
    top_output = output;
    bool slow = blanks - top_blank >= 2;
    top_blank = blanks;
    between_ready = false;
    bool drew = std::any_of(ops_now.begin(), ops_now.end(), [](const Op &op) { return op.kind == Op::Draw; });
    if (drew && slow && last_pica) {
        draw_between(0.5f);
        between_ready = true;
        between_address = output;
    }
    // each target's draws, as the next frame to draw it matches them
    std::map<uint32_t, std::vector<Op>> drawn;
    for (Op &op : ops_now) {
        if (op.kind == Op::Draw) drawn[op_target(op)].push_back(std::move(op));
    }
    for (auto &[target, list] : drawn) draws_before[target] = std::move(list);
    drop_ops();
}

void Impl::drop_ops() {
    ops_now.clear();
    dead.insert(dead.end(), dead_held.begin(), dead_held.end());
    dead_held.clear();
}

// an image let go: kept while a frame's ops may still draw with it
void Impl::let_go(const Image &i) { (between && !replaying ? dead_held : dead).push_back(i); }

} // namespace runtime
