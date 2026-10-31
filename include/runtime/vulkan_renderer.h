// The runtime's renderer on the host GPU through Vulkan: the PICA's draws
// as Vulkan draws, its buffers kept as images until something else needs
// them in memory (docs/vulkan-renderer.md).
#ifndef RUNTIME_VULKAN_RENDERER_H
#define RUNTIME_VULKAN_RENDERER_H

#include "runtime/pica.h"

#include <memory>
#include <string>

namespace runtime {

class VulkanRenderer : public pica::Backend {
public:
    explicit VulkanRenderer(Memory &memory);
    ~VulkanRenderer() override;
    // false when there is no Vulkan 1.3 device; error says why
    bool ready() const;
    std::string error;

    void draw(pica::Pica &pica, bool indexed) override;
    void draw_immediate(pica::Pica &pica, const std::vector<std::array<pica::Vec4, 16>> &vertices) override;
    void sync(uint32_t paddr, uint32_t size, bool write) override;
    void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) override;
    bool display_transfer(const Transfer &transfer) override;
    bool picture(uint32_t screen, uint32_t address, uint32_t stride, uint32_t format, Picture &out) override;
    void frame_done() override;

    // counters
    uint64_t draws = 0, triangles = 0, uploads = 0, write_backs = 0, submits = 0;
    // where the time went, in seconds: the vertex stage, recording draws, syncs
    double time_vertices = 0, time_render = 0, time_sync = 0;
    // write-backs by cause: the CPU (guards), the front end, a fill, a texture
    uint64_t write_back_causes[4]{};
    // how much of each game frame matches the one before, draw for draw (the
    // ground for interpolated frames): with count_shapes, the frames seen,
    // their draws, those matched, and those of them that moved
    bool count_shapes = false;
    // frames drawn between two of the game's, its logic untouched: each
    // draw's vertices placed between the frame before and this one (1 for
    // now, halfway). each game frame is drawn again once it is done
    int between = 0;
    // a screen of the last frame drawn between, as picture() gives the game's
    bool picture_between(uint32_t screen, uint32_t address, uint32_t stride, uint32_t format, Picture &out);
    // the game frame done so far drawn between, before the screens are taken
    // (frame_done does it otherwise); game_frames counts the frames that went
    // to a screen screen_rows tall (400 the top one: a display transfer there)
    void prepare_between(uint32_t screen_rows = 400);
    uint64_t game_frames = 0;
    uint64_t frames_seen = 0, draws_seen = 0, draws_matched = 0, draws_moved = 0;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool prepared_ = false;
};

} // namespace runtime

#endif
