// The runtime's renderer on the host GPU through Vulkan: the PICA's draws
// as Vulkan draws, its buffers kept as images until something else needs
// them in memory (docs/vulkan-renderer.md).
#ifndef RUNTIME_VULKAN_RENDERER_H
#define RUNTIME_VULKAN_RENDERER_H

#include "runtime/pica.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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
        // draws with a fragment shader made for their state's shape
        uint64_t specialized_draws = 0;
        // draws between paired with one of the same tag (a port's code
        // tags the game's draws)
        uint64_t tagged_draws = 0;
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
    // images for textures: a PNG read into RGBA8 rows (the first row the
    // top), and written from them; the front end's, with what it links
    using LoadImage = std::function<bool(const std::filesystem::path &, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h)>;
    using SaveImage = std::function<bool(const std::filesystem::path &, const uint8_t *rgba, uint32_t w, uint32_t h)>;
    // textures of our own (HD textures) in place of the game's: the PNGs and
    // DDSs (BC1, BC3, BC7, kept compressed; their rows bottom first, as the
    // PICA keeps them) in folder and its folders, each named with the 16 hex
    // digits of the hash of the texture it replaces (hash_bytes over its first
    // level as memory has it, as tools/texdump.cpp names them), of any size. read as the
    // game first uses them, or, preload, all of them from the start on
    // threads of their own (the game asking for one not read yet reads it
    // itself); before the game starts
    void textures_from(const std::filesystem::path &folder, LoadImage load, bool preload = false);
    // every texture the game uses written to folder once, named as texdump
    // names them (those only the running game makes too: the font, what it
    // draws into and reads back); before the game starts
    void dump_textures(const std::filesystem::path &folder, SaveImage save);
    // frames drawn between two of the game's, its logic untouched: each
    // draw's vertices placed between where the frame before and this one
    // put them. the pictures each vertical blank shows: 0 none drawn, 1 (a
    // scene at 30 shown at 60), 2 (at 120, a scene at 60 too), up to 4. set
    // from any thread, taken at the next vertical blank
    std::atomic<int> between{0};
    // debugging the frames between: drawn again as they are, every vertex
    // where the game's own frame has it (no motion), to tell a glitch of the
    // drawing again from one of the motion. set from any thread
    std::atomic<bool> between_still{false};
    // the resolution: how many of the host's pixels each of the PICA's is,
    // each way (fractions too: 4.5 makes the top screen 1080 rows tall). its
    // buffers are drawn that large; memory keeps them as the console does.
    // set from any thread, taken at the next vertical blank (the buffers go
    // back to memory and come again at the new size)
    std::atomic<float> scale{1};
    // the outlines games draw by filtering for edges (USUM's around its
    // characters, a texel of its own wide, thick at a high resolution):
    // 0 as drawn, 1 thin (a pixel of the host's at any resolution), 2 none.
    // set from any thread
    std::atomic<int> outlines{0};
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
