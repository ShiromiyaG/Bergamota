// The runtime's renderer on the host GPU through Vulkan: the PICA's draws
// as Vulkan draws, its buffers kept as images until something else needs
// them in memory (docs/vulkan-renderer.md).
#ifndef RUNTIME_VULKAN_RENDERER_H
#define RUNTIME_VULKAN_RENDERER_H

#include "runtime/pica.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace runtime {

class VulkanRenderer : public pica::Backend {
public:
    // instance_extensions: what a window's presenter needs (SDL_Vulkan_GetInstanceExtensions);
    // with them the device can present as well
    explicit VulkanRenderer(Memory &memory, const std::vector<std::string> &instance_extensions = {});
    ~VulkanRenderer() override;
    // false when there is no Vulkan 1.3 device; error says why
    bool ready() const;
    std::string error;

    void draw(pica::Pica &pica, bool indexed) override;
    void draw_immediate(pica::Pica &pica, const std::vector<std::array<pica::Vec4, 16>> &vertices) override;
    void sync(uint32_t paddr, uint32_t size, bool write) override;
    void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) override;
    bool display_transfer(const Transfer &transfer) override;
    bool vblank(pica::Pica &pica) override;

    // what it did and where the time went, for boot and the player's
    // RUNTIME_STATS
    struct Stats {
        uint64_t draws = 0, triangles = 0, uploads = 0, write_backs = 0, submits = 0;
        // where the time went, in seconds: the vertex stage, recording draws, syncs
        double time_vertices = 0, time_render = 0, time_sync = 0;
        // the host GPU's own time running what was sent, in seconds
        double gpu_seconds = 0;
        // of it: the passes that draw (the frames between's among them), the
        // frames between's copies of the buffers, the frames between whole, the
        // screens' copies
        double gpu_parts[4]{};
        // render passes, and why they ended: another target, a texture that was
        // the target, the rest
        uint64_t passes = 0, pass_ends[3]{};
        // draws with the game's vertex program translated, and the programs
        // translated, taken from the cache, and failed
        uint64_t translated_draws = 0;
        // draws between taken from the outputs the game's draws kept
        uint64_t reused_draws = 0;
        // draws asking for what the renderer does not do: gas (Azahar has
        // none either), a geometry shader mode past the three there are
        uint64_t lacking[2]{};
        // write-backs by cause: the CPU (guards), the front end, a fill, a texture
        uint64_t write_back_causes[4]{};
    } stats;
    void translation_counts(uint64_t &compiled, uint64_t &loaded, uint64_t &failed) const;
    // the translated shaders and the driver's pipelines kept in folder
    // across runs; before the game starts
    void cache_in(const std::string &folder);
    // frames drawn between two of the game's, its logic untouched: each
    // draw's vertices placed halfway between the frame before and this one,
    // so a scene at 30 shows at 60. 0 or 1; set from any thread, taken at
    // the next vertical blank
    std::atomic<int> between{0};
    // the resolution: how many of the host's pixels each of the PICA's is,
    // each way (fractions too: 4.5 makes the top screen 1080 rows tall). its
    // buffers are drawn that large; memory keeps them as the console does.
    // set from any thread, taken at the next vertical blank (the buffers go
    // back to memory and come again at the new size)
    std::atomic<float> scale{1};
    // the top screen shown this much wider than it is drawn (widescreen,
    // anamorphic: a port's override frames its 3D wider): what is drawn
    // flat on it (2D, every w 1) goes narrower by as much, centered, so it
    // keeps its shape once stretched. 1 for none; set from any thread
    std::atomic<float> top_wide{1};
    // the frames' pictures read back to the CPU (boot's shots, a dump):
    // without, only the GPU's copies for a presenter
    std::atomic<bool> cpu_pictures{true};

    struct Impl;

private:
    friend class VulkanPresenter;
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime

#endif
