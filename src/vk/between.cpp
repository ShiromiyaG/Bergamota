// Frames drawn between the game's: each frame's draws kept and drawn again
// with every vertex between the frame before and this one, as many times as
// the blanks it took have pictures. (impl.h)

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
    if (gpu_draw) {
        // its inputs, which are the same in a thing that only moves
        auto kept = std::make_shared<GpuDraw>(*gpu_draw);
        kept->input_hash = hash_bytes(reinterpret_cast<const uint8_t *>(kept->inputs.data()), kept->inputs.size() * sizeof(Vec4)) ^
                           hash_bytes(reinterpret_cast<const uint8_t *>(kept->indices.data()), kept->indices.size() * 4) * 3 ^
                           kept->stride * 0x9E3779B97F4A7C15ull;
        op.uvs = kept->input_hash;
        op.gpu = std::move(kept);
    }
    ops_now.push_back(std::move(op));
}

uint32_t Impl::op_target(const Op &op) { return (op.regs[COLOR_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3; }

// a draw of the frame before the same as op could be: shaded on the GPU,
// the same inputs and program; on the CPU, as many vertices; and the same
// look either way
static bool alike(const Impl::Op &before, const Impl::Op &op) {
    if (before.look != op.look) return false;
    if (op.gpu) {
        return before.gpu && before.gpu->input_hash == op.gpu->input_hash && before.gpu->program_hash == op.gpu->program_hash;
    }
    return !before.gpu && before.vertices.size() == op.vertices.size();
}

// the guess from the draws alone (match): its index in list, or -1
int Impl::guess(const std::vector<Op> &list, const Matching &m, const Op &op) {
    if (op.gpu) {
        // shaded on the GPU: the one with the same inputs and program whose
        // uniforms differ the least (quads alike in all but where they go)
        int best = -1;
        float least = 0;
        for (size_t i = 0; i < list.size(); i++) {
            const Op &before = list[i];
            if (m.used[i] || !alike(before, op)) continue;
            float d = 0;
            const float *a = &before.gpu->unit.f[0][0], *b = &op.gpu->unit.f[0][0];
            for (int k = 0; k < 96 * 4; k++) d += std::abs(a[k] - b[k]);
            if (best < 0 || d < least) best = int(i), least = d;
        }
        return best;
    }
    int same = -1;
    float least = 0.5f; // past it, a jump
    for (size_t i = 0; i < list.size(); i++) {
        const Op &before = list[i];
        if (m.used[i] || before.uvs != op.uvs || !alike(before, op)) continue;
        if (float d = moved(before.vertices, op.vertices); d < least) least = d, same = int(i);
    }
    int best = same;
    if (best < 0) {
        size_t start = m.from > 8 ? m.from - 8 : 0, end = std::min(list.size(), m.from + 48);
        float nearest = 0.25f; // an eighth of the screen
        int in_order = -1;
        for (size_t i = start; i < end; i++) {
            const Op &before = list[i];
            if (m.used[i] || !alike(before, op)) continue;
            float d = moved(before.vertices, op.vertices);
            if (d >= 0.25f) continue;
            if (in_order < 0 && i >= m.from) in_order = int(i);
            if (d < nearest) nearest = d, best = int(i);
        }
        if (in_order >= 0) best = in_order;
    }
    return best;
}

const Impl::Op *Impl::match(const std::vector<Op> &list, Matching &m, const Op &op) {
    if (m.used.size() != list.size()) m.used.assign(list.size(), false);
    int best = guess(list, m, op);
    // a draw the game tagged with its model's mesh (DRAW_TAG): the next of
    // that mesh's draws the frame before, the guess counted against it; a
    // mesh not drawn before has none. A tag is a draw tag's address, which
    // a model made after another was let go can take over (NPCs come and go
    // as the field scrolls): where the two disagree, the pair that moved
    // the least, and a tagged one the guess found none for only if it moved
    // little, as the guess asks (else a shadow drawn between two NPCs
    // flashes up halfway)
    if (uint32_t tag = op.regs[DRAW_TAG]) {
        int tagged = -1;
        for (size_t i = 0; i < list.size() && tagged < 0; i++) {
            if (!m.used[i] && list[i].regs[DRAW_TAG] == tag && alike(list[i], op)) tagged = int(i);
        }
        m.tagged++;
        if (tagged != best) {
            m.disagreed++;
            if (tagged >= 0) {
                float by_tag = distance(list[tagged], op);
                if (best < 0 ? by_tag < 0.25f : by_tag <= distance(list[best], op)) best = tagged;
            } else {
                best = -1;
            }
        }
    }
    if (best < 0) return nullptr;
    m.used[best] = true;
    m.from = size_t(best) + 1;
    return &list[best];
}

// how large a draw is on the screen, in halves of it as moved counts: the
// larger side of what its vertices in front of the eye cover, 2 when none
float Impl::extent(const std::vector<Vertex> &vertices) {
    float far = 0;
    for (const Vertex &v : vertices) far = std::max(far, v.position[3]);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Vertex &v : vertices) {
        const Vec4 &p = v.position;
        if (p[3] <= far * 0.05f) continue;
        float x = p[0] / p[3], y = p[1] / p[3];
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (x1 < x0) return 2;
    return std::max(x1 - x0, y1 - y0);
}

// how far a draw moved from one of the frame before, as moved has it:
// one shaded on the GPU by a few of its vertices run here (jumps' samples)
float Impl::distance(const Op &before, const Op &now) {
    if (!now.gpu) return moved(before.vertices, now.vertices);
    if (!before.gpu) return 2;
    for (GpuDraw *d : {before.gpu.get(), now.gpu.get()}) {
        if (d->samples.empty()) d->samples = sample_positions(*d);
    }
    if (before.gpu->samples.size() != now.gpu->samples.size()) return 2;
    return moved(before.gpu->samples, now.gpu->samples);
}

// the most a vertex moved, in halves of the screen. only the vertices
// well in front of the eye and on the screen or near it count: near the
// eye, or far off the screen, a small move projects to a huge one (the
// ground reaching under the camera, a big piece mostly off the screen)
float Impl::moved(const std::vector<Vertex> &before, const std::vector<Vertex> &now) {
    float far = 0;
    for (size_t i = 0; i < now.size(); i++) far = std::max({far, before[i].position[3], now[i].position[3]});
    const float near = far * 0.05f;
    float most = 0;
    for (size_t i = 0; i < now.size(); i++) {
        const Vec4 &a = before[i].position, &b = now[i].position;
        if (a[3] <= near || b[3] <= near) continue;
        float ax = a[0] / a[3], ay = a[1] / a[3], bx = b[0] / b[3], by = b[1] / b[3];
        bool on_screen = (std::abs(ax) <= 1.5f && std::abs(ay) <= 1.5f) || (std::abs(bx) <= 1.5f && std::abs(by) <= 1.5f);
        if (!on_screen) continue;
        most = std::max({most, std::abs(ax - bx), std::abs(ay - by)});
    }
    return most;
}

// SAMPLES vertices spread over the draw, through its program on the CPU:
// a cut moves them all alike, so a few tell it
std::vector<Vertex> Impl::sample_positions(const GpuDraw &d) {
    constexpr size_t SAMPLES = 8;
    const size_t count = d.stride ? d.inputs.size() / d.stride : 0;
    std::vector<Vertex> out(std::min(count, SAMPLES));
    if (out.empty() || !d.program) return out;
    // the programs decoded, by hash: the same few each frame
    thread_local std::unordered_map<uint64_t, std::unique_ptr<std::pair<ShaderUnit, Program>>> decoded;
    if (decoded.size() > 256 && !decoded.count(d.program_hash)) decoded.clear();
    auto &slot = decoded[d.program_hash];
    if (!slot) {
        slot = std::make_unique<std::pair<ShaderUnit, Program>>();
        std::memcpy(slot->first.program.data(), d.program->code, sizeof d.program->code);
        std::memcpy(slot->first.descriptors.data(), d.program->desc, sizeof d.program->desc);
        slot->second.decode(slot->first);
    }
    ShaderUnit &u = slot->first;
    std::memcpy(u.floats.data(), d.unit.f, sizeof d.unit.f);
    for (int k = 0; k < 4; k++) {
        for (int c = 0; c < 4; c++) u.ints[k][c] = uint8_t(d.unit.i[k][c]);
    }
    u.bools = uint16_t(d.unit.misc[0]);
    u.entry = d.unit.misc[1];
    const uint32_t mask = d.unit.misc[2] & 0xFFFF, total = std::min(d.unit.misc[3] & 7, 7u);
    Runner runner;
    for (size_t s = 0; s < out.size(); s++) {
        const size_t v = count * s / out.size();
        runner.input = {};
        for (uint32_t r = 0; r < d.stride && r < 16; r++) runner.input[r] = d.inputs[v * d.stride + r];
        runner.run(u, slot->second);
        // the output registers packed by the mask, then placed by the map
        // (pica_gpu.vert, place)
        Vec4 packed[16]{};
        for (uint32_t r = 0, at = 0; r < 16; r++) {
            if (!mask) packed[r] = runner.output[r];
            else if (mask >> r & 1) packed[at++] = runner.output[r];
        }
        // the position, and the texture coordinates (the outlines' edge filter, draw.cpp)
        float values[24] = {};
        values[3] = 1;
        for (uint32_t sl = 0; sl < total; sl++) {
            for (int c = 0; c < 4; c++) {
                if (uint32_t semantic = d.unit.map[sl] >> (c * 8) & 0x1F; semantic < 24) values[semantic] = packed[sl][c];
            }
        }
        if (!total) {
            for (int c = 0; c < 4; c++) values[c] = packed[0][c];
            values[12] = packed[3][0], values[13] = packed[3][1];
        }
        Vertex &sample = out[s];
        sample.position = {values[0], values[1], values[2], values[3]};
        sample.uv0[0] = values[12], sample.uv0[1] = values[13];
        sample.uv1[0] = values[14], sample.uv1[1] = values[15];
        sample.uv2[0] = values[22], sample.uv2[1] = values[23];
    }
    return out;
}

// a pair that jumps (more than a quarter of the screen) is not the same
// thing moving (a camera cut): drawn where it is now. one shaded on the
// GPU tells it from a few vertices run here, when its uniforms changed
// a small thing that moved more than its own size is another one (NPCs'
// shadows, alike quads: one gone and one come paired, drawn halfway, would
// flash up between them): past the larger of its size and a fortieth of the
// screen, up to the quarter that tells a cut
static float jump_past(const std::vector<Vertex> &now) {
    return std::clamp(Impl::extent(now), 0.05f, 0.5f);
}

bool Impl::jumps(const Op &before, const Op &now) {
    if (!now.gpu) return moved(before.vertices, now.vertices) > jump_past(now.vertices);
    if (!before.gpu || std::memcmp(&before.gpu->unit, &now.gpu->unit, sizeof(UnitBlock)) == 0) return false;
    // each once: this frame's are the next one's frame before
    for (GpuDraw *d : {before.gpu.get(), now.gpu.get()}) {
        if (d->samples.empty()) d->samples = sample_positions(*d);
    }
    return moved(before.gpu->samples, now.gpu->samples) > jump_past(now.gpu->samples);
}

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
    int whole = gpu_open(2), seeding = gpu_open(1);
    for (auto &s : surfaces) {
        kept.push_back({s->dirty, s->stale, s->version, s->converted});
        if (!s->between.image) s->between = image(s->image.width, s->image.height, s->depth ? depth_format : VK_FORMAT_R8G8B8A8_UNORM, s->depth);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {
            VkImageAspectFlags(s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        c.extent = {s->image.width, s->image.height, 1};
        vkCmdCopyImage(cmd, s->image.image, VK_IMAGE_LAYOUT_GENERAL, s->between.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        between_views[s->image.view] = s->between.view;
        std::swap(s->image, s->between);
    }
    barrier();
    gpu_close(seeding);
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
        if (op.gpu) {
            // the GPU's shader runs both and puts each vertex between
            gpu_draw = op.gpu.get();
            gpu_before = before && !jumps(*before, op) ? before->gpu.get() : nullptr;
            gpu_t = t;
        } else if (before && !jumps(*before, op)) {
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
        gpu_draw = gpu_before = nullptr;
        gpu_t = 1;
    }
    end_rendering();
    forced_views = nullptr;
    replaying = false;
    for (auto &[target, m] : matched) self.stats.tagged_draws += m.tagged, self.stats.tags_disagreed += m.disagreed;
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
    gpu_close(whole);
}

// the top screen's buffer at output as the frame between just drew it,
// kept as picture slot for the blanks to show
bool Impl::keep_between(uint32_t output, int slot) {
    Surface *fb = nullptr;
    for (auto &s : surfaces) {
        if (s->linear && !s->depth && s->address == output && s->between.image) fb = s.get();
    }
    if (!fb) return false;
    auto &list = between_pictures[between_list];
    if (list.size() <= size_t(slot)) list.resize(size_t(slot) + 1);
    Image &to = list[slot];
    if (to.image && (to.width != fb->between.width || to.height != fb->between.height)) let_go(to), to = {};
    if (!to.image) to = image(fb->between.width, fb->between.height, VK_FORMAT_R8G8B8A8_UNORM, false);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {to.width, to.height, 1};
    vkCmdCopyImage(cmd, fb->between.image, VK_IMAGE_LAYOUT_GENERAL, to.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier();
    return true;
}

// a frame for the top screen ended in a transfer into output: drawn
// between as many times as the blanks since the one before show pictures
// (`between` a blank, the last the game's own: one for a scene at 30 shown
// at 60, none for one at 60 unless more than 60), then kept as the frame
// before
void Impl::frame_end(uint32_t output) {
    top_frames++;
    top_output = output;
    const int span = int(std::min<uint64_t>(blanks - top_blank, 3));
    top_blank = blanks;
    between_ready = false;
    bool drew = std::any_of(ops_now.begin(), ops_now.end(), [](const Op &op) { return op.kind == Op::Draw; });
    const int pictures = span * std::clamp(between, 0, PARTS);
    if (drew && pictures > 1 && last_pica) {
        bool kept = true;
        between_list ^= 1;
        for (int k = 1; k < pictures; k++) {
            draw_between(float(k) / float(pictures));
            kept = keep_between(output, k - 1) && kept;
        }
        between_ready = kept;
        between_address = output;
        between_count = pictures - 1;
        between_blanks = span;
    }
    // the next frame's outputs go to the other half; this one's stay as
    // the frame before's
    output_turn++;
    output_at = 0;
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
