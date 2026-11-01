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

// how unlike a draw of the frame before one of this frame's is, to pair
// them (pair_frame), lower the likelier; below 0 not a pair at all. Shaded
// on the GPU: how far their uniforms are apart (alike in all but where
// they go). On the CPU: how far its vertices moved, those that sample the
// same part of their first texture first (the same thing, moved; a curb's
// pieces each sample their own), up to half the screen, else a quarter.
// Tags of two meshes never pair; the same tag is likelier
static float unlike(const Impl::Op &before, const Impl::Op &now) {
    if (!alike(before, now)) return -1;
    uint32_t tag = now.regs[DRAW_TAG], was = before.regs[DRAW_TAG];
    if (tag && was && tag != was) return -1;
    float score;
    if (now.gpu) {
        score = 0;
        const float *a = &before.gpu->unit.f[0][0], *b = &now.gpu->unit.f[0][0];
        for (int k = 0; k < 96 * 4; k++) score += std::abs(a[k] - b[k]);
    } else {
        const bool same = before.uvs == now.uvs;
        float d = Impl::moved(before.vertices, now.vertices);
        if (d >= (same ? 0.5f : 0.25f)) return -1;
        score = d + (same ? 0.0f : 1.0f);
    }
    return tag && tag == was ? score * 0.5f : score;
}

// each of the frame's draws paired with the one of the frame before it is
// (pairs, by its place in ops_now; null for none), for the frame as a
// whole: of all the pairs a target's draws could make, the likeliest first,
// each draw taken once. Paired one at a time in the game's order, a
// character coming in (drawn before another of its model) took the other's
// draw of the frame before, was drawn halfway from it, and flashed; the
// other went unpaired. A draw is tried against those near its place in the
// order (64 either way, scaled) when there are many alike
void Impl::pair_frame() {
    pairs.assign(ops_now.size(), nullptr);
    std::map<uint32_t, std::vector<size_t>> by_target;
    for (size_t i = 0; i < ops_now.size(); i++) {
        if (ops_now[i].kind == Op::Draw) by_target[op_target(ops_now[i])].push_back(i);
    }
    struct Candidate {
        float score;
        size_t now, before;
    };
    std::vector<Candidate> candidates;
    std::vector<bool> taken;
    for (auto &[target, now] : by_target) {
        auto last = draws_before.find(target);
        if (last == draws_before.end()) continue;
        const std::vector<Op> &list = last->second;
        candidates.clear();
        for (size_t k = 0; k < now.size(); k++) {
            const Op &op = ops_now[now[k]];
            size_t at = list.size() * k / now.size(), from = at > 64 ? at - 64 : 0, to = std::min(list.size(), at + 65);
            if (now.size() * list.size() <= 64 * 64) from = 0, to = list.size();
            for (size_t j = from; j < to; j++) {
                float score = unlike(list[j], op);
                if (score >= 0) candidates.push_back({score, now[k], j});
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score < b.score; });
        taken.assign(list.size(), false);
        for (const Candidate &c : candidates) {
            if (pairs[c.now] || taken[c.before]) continue;
            pairs[c.now] = &list[c.before];
            taken[c.before] = true;
            if (ops_now[c.now].regs[DRAW_TAG]) self.stats.tagged_draws++;
        }
    }
}

// how far one vertex of a pair may move before it is not the same point:
// sixteen times what the draw's vertices moved as a rule (the median, those
// in front of the eye), a quarter of the screen at least: decided a vertex
// at a time on the GPU, a triangle split by it would stretch (a cutscene's
// camera sweeping past things near it). One far past the
// rest is another thing's (a batch reordered); parallax and a swinging arm
// stay well within it
float Impl::jump_limit(const std::vector<Vertex> &before, const std::vector<Vertex> &now) {
    std::vector<float> moves;
    for (size_t i = 0; i < now.size() && i < before.size(); i++) {
        const Vec4 &a = before[i].position, &b = now[i].position;
        if (a[3] <= 0 || b[3] <= 0) continue;
        moves.push_back(std::max(std::abs(a[0] / a[3] - b[0] / b[3]), std::abs(a[1] / a[3] - b[1] / b[3])));
    }
    if (moves.empty()) return 1e9f;
    std::nth_element(moves.begin(), moves.begin() + moves.size() / 2, moves.end());
    return std::max(moves[moves.size() / 2] * 16, 0.5f);
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
bool Impl::jumps(const Op &before, const Op &now) {
    if (!now.gpu) return moved(before.vertices, now.vertices) > 0.5f;
    if (!before.gpu || std::memcmp(&before.gpu->unit, &now.gpu->unit, sizeof(UnitBlock)) == 0) return false;
    // each once: this frame's are the next one's frame before
    for (GpuDraw *g : {before.gpu.get(), now.gpu.get()}) {
        if (g->samples.empty()) g->samples = sample_positions(*g);
    }
    return moved(before.gpu->samples, now.gpu->samples) > 0.5f;
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
    // the buffers, those the frame let go of too (retired)
    std::vector<Surface *> all;
    for (auto &s : surfaces) all.push_back(s.get());
    for (auto &s : retired) all.push_back(s.get());
    // a depth buffer read as color (an effect reading the depth): made
    // again from the between depth when a replayed draw reads it
    std::unordered_map<VkImageView, Surface *> depth_views;
    std::vector<Surface *> depth_redone;
    for (Surface *s : all) {
        if (s->as_color.image) depth_views[s->as_color.view] = s;
    }
    int whole = gpu_open(2), seeding = gpu_open(1);
    for (Surface *s : all) {
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
    // one way for all the draws (impl.h, run_all)
    run_all = outputs_full;
    for (size_t index = 0; index < ops_now.size() && !run_all; index++) {
        const Op &op = ops_now[index];
        const Op *before = index < pairs.size() ? pairs[index] : nullptr;
        if (op.kind != Op::Draw || !op.gpu) continue;
        bool kept = op.gpu->outputs != ~0u && op.gpu->output_turn == output_turn;
        if (before && before->gpu) kept = kept && before->gpu->outputs != ~0u && before->gpu->output_turn + 1 == output_turn;
        run_all = !kept;
    }
    for (size_t index = 0; index < ops_now.size(); index++) {
        Op &op = ops_now[index];
        if (op.kind == Op::Fill) {
            fill(op.fill[0], op.fill[1], op.fill[2], op.fill[3]);
            continue;
        }
        if (op.kind == Op::Transfer) {
            transfer(op.transfer);
            continue;
        }
        const Op *before = index < pairs.size() ? pairs[index] : nullptr;
        triangles = op.vertices;
        if (op.gpu) {
            // the GPU's shader runs both and puts each vertex between, those
            // past the limit its few vertices run here give left where they are
            gpu_draw = op.gpu.get();
            gpu_before = before && !jumps(*before, op) ? before->gpu.get() : nullptr;
            gpu_t = t;
            gpu_jump = gpu_before && !gpu_before->samples.empty() && !op.gpu->samples.empty()
                           ? jump_limit(gpu_before->samples, op.gpu->samples) : 1e9f;
        } else if (before && !jumps(*before, op)) {
            // a triangle with a vertex far past the rest's moves (another
            // thing's, a batch reordered), or one that crossed the eye's
            // plane (mixed, it would pass through infinity), is left where
            // it is, whole
            const float limit = jump_limit(before->vertices, op.vertices);
            for (size_t i = 0; i < triangles.size(); i += 3) {
                size_t end = std::min(i + 3, triangles.size());
                bool keep = false;
                for (size_t j = i; j < end; j++) {
                    const Vec4 &a = before->vertices[j].position, &b = op.vertices[j].position;
                    if (a[3] * b[3] <= 0) keep = true;
                    else if (a[3] > 0 && std::max(std::abs(a[0] / a[3] - b[0] / b[3]), std::abs(a[1] / a[3] - b[1] / b[3])) > limit) keep = true;
                }
                if (keep) continue;
                for (size_t j = i; j < end; j++) {
                    const Vec4 &a = before->vertices[j].position, &b = op.vertices[j].position;
                    for (int k = 0; k < 4; k++) triangles[j].position[k] = a[k] + (b[k] - a[k]) * t;
                }
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
        gpu_t = 1, gpu_jump = 1e9f;
    }
    end_rendering();
    forced_views = nullptr;
    replaying = false;
    p.regs = regs;
    p.light_tables = tables;
    p.tables_version = tables_version;
    for (size_t i = 0; i < all.size(); i++) {
        Surface &s = *all[i];
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
        pair_frame();
        bool kept = true;
        between_list ^= 1;
        for (int k = 1; k < pictures; k++) {
            draw_between(self.between_still.load() ? 1.0f : float(k) / float(pictures));
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
    outputs_full = false;
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
    // the buffers let go of during the frame, drawn between by now
    for (auto &s : retired) {
        let_go(s->image);
        if (s->as_color.image) let_go(s->as_color);
        if (s->between.image) let_go(s->between);
    }
    retired.clear();
}

// an image let go: kept while a frame's ops may still draw with it
void Impl::let_go(const Image &i) { (between && !replaying ? dead_held : dead).push_back(i); }

} // namespace runtime
