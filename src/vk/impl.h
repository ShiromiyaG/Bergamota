// What the Vulkan renderer's files share (renderer.cpp and the rest of
// src/vk/): the buffers and images it keeps, and Impl, split by job:
// device.cpp (Vulkan itself, submitting), surfaces.cpp (the PICA's buffers
// as images), textures.cpp, draw.cpp (pipelines, a draw), between.cpp (frames
// drawn between the game's) and screens.cpp (what the screens show).
#ifndef RUNTIME_VK_IMPL_H
#define RUNTIME_VK_IMPL_H

#include "runtime/vulkan_renderer.h"
#include "runtime/memory.h"
#include "runtime/pica_lighting.h"
#include "../pica/texture.h"
#include "translate.h"
#include "../pica/vertex.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <atomic>
#include <mutex>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace runtime {

using namespace pica;

constexpr VkDeviceSize VERTEX_RING = 64u << 20, UNIFORM_RING = 32u << 20;

// the PICA's vertex shader on the GPU (pica_gpu.vert): the program and its
// descriptors, and a draw's uniforms and output map, as the shader reads them
struct ProgramBlock {
    uint32_t code[4096];
    uint32_t desc[128];
};
struct UnitBlock {
    float f[96][4];
    int32_t i[4][4];   // the integer uniforms' bytes
    uint32_t misc[4];  // bools, entry, output mask, output total
    uint32_t map[8];   // the output map's seven words
};
struct VertexPush {
    uint32_t stride, before_stride;
    float t;
    uint32_t between;
    uint32_t now_out, before_out, store;
    float jump; // a vertex that moved more (on the screen, -1 to 1) stays where it is now
};
// a draw for the GPU's vertex shader: its inputs, triangles as indices,
// uniforms and program
struct GpuDraw {
    std::vector<Vec4> inputs;
    uint32_t stride = 0;
    std::vector<uint32_t> indices;
    UnitBlock unit{};
    std::shared_ptr<const ProgramBlock> program;
    uint64_t program_hash = 0;
    uint64_t input_hash = 0; // the inputs, the indices and the stride: alike in a thing that only moves
    // where its vertices' outputs went (pica_gpu.vert, outputs), in floats,
    // and in which half's turn; none when out of room
    uint32_t outputs = ~0u;
    uint64_t output_turn = 0;
    // a few of its positions run on the CPU, for the jump test (between.cpp)
    std::vector<Vertex> samples;
};

// w x h values resampled to pw x ph, each taking the nearest (the buffers
// at another resolution than memory's)
template <typename T> std::vector<T> resampled(const std::vector<T> &in, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph, uint32_t n = 1) {
    if (pw == w && ph == h) return in;
    std::vector<T> out(size_t(pw) * ph * n);
    for (uint32_t y = 0; y < ph; y++) {
        uint32_t sy = std::min(h - 1, uint32_t(uint64_t(y) * h / ph));
        for (uint32_t x = 0; x < pw; x++) {
            uint32_t sx = std::min(w - 1, uint32_t(uint64_t(x) * w / pw));
            std::copy_n(&in[(size_t(sy) * w + sx) * n], n, &out[(size_t(y) * pw + x) * n]);
        }
    }
    return out;
}

// where a logical pixel's middle falls among n physical ones over count logical
inline uint32_t middle(uint32_t at, uint32_t count, uint32_t n) { return std::min(n - 1, uint32_t((2 * uint64_t(at) + 1) * n / (2 * count))); }

// the registers the shaders read, by slot (pica.frag)
struct State {
    uint32_t regs[64];
    float depth[4];
};
// a register no game's GPU uses: what a draw is, as a port's override tags
// it (between.cpp, match)
constexpr uint32_t DRAW_TAG = 0x00F;
constexpr uint32_t STATE_REGS[] = {
    0x0C0, 0x0C1, 0x0C2, 0x0C3, 0x0C4, 0x0C8, 0x0C9, 0x0CA, 0x0CB, 0x0CC, 0x0D0, 0x0D1, 0x0D2, 0x0D3, 0x0D4,
    0x0D8, 0x0D9, 0x0DA, 0x0DB, 0x0DC, 0x0F0, 0x0F1, 0x0F2, 0x0F3, 0x0F4, 0x0F8, 0x0F9, 0x0FA, 0x0FB, 0x0FC,
    0x0E0, 0x0FD, 0x104, 0x080, 0x083, 0x093, 0x09B, 0x081, 0x091, 0x099, 0x0E1,
    0x0A8, 0x0A9, 0x0AA, 0x0AB, 0x0AC, 0x0AD,
    0x08B, 0x130, // texture 0's shadow (perspective, bias); the shadow output's constant and linear
};

// a texture unit's registers: border color, then size, parameters, address
// and format
constexpr uint32_t UNIT_BASE[3] = {0x081, 0x091, 0x099};
inline uint32_t unit_format(const Pica &p, int unit) { return p.regs[unit ? UNIT_BASE[unit] + 5 : 0x08E] & 15; }

// four lanes side by side, which the CPU runs at memory speed
inline uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t lane[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            std::memcpy(&w, p + i + k * 8, 8);
            lane[k] = (lane[k] ^ w) * 0x9E3779B97F4A7C15ull;
            lane[k] ^= lane[k] >> 31;
        }
    }
    uint64_t h = lane[0] ^ lane[1] * 3 ^ lane[2] * 5 ^ lane[3] * 7 ^ n;
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE, stencil = VK_NULL_HANDLE; // a depth image's aspects, to sample
    VkImageView words = VK_NULL_HANDLE; // an RGBA8 image as R32_UINT, made when a shadow map needs it
    uint32_t width = 0, height = 0;
};

// a color or depth buffer of the PICA's, on the GPU
struct Surface {
    uint32_t address, width, height, format;
    bool depth;
    bool linear = false; // rows in order, as a display transfer leaves a framebuffer
    Image image;
    bool dirty = false; // the GPU has drawn what memory has not
    bool stale = false; // memory was written since: loaded again before use
    // a depth buffer read as a color texture: made from it when it moved on
    uint64_t version = 0, converted = ~0ull;
    Image as_color;
    // the same buffer in a frame drawn between the game's (interpolation):
    // swapped in for image while the frame is drawn again
    Image between;
    uint32_t offset(uint32_t x, uint32_t y, uint32_t bpp) const {
        return linear ? (y * width + x) * bpp : tiled_offset(x, y, width, bpp);
    }
    uint32_t bytes() const {
        static constexpr uint32_t DEPTH_BYTES[4] = {2, 2, 3, 4};
        return width * height * (depth ? DEPTH_BYTES[format & 3] : bytes_per_pixel(format));
    }
};

struct Texture {
    Image image;
    uint64_t hash = 0, checked_list = ~0ull;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t *mapped = nullptr;
    VkDeviceSize size = 0;
    bool read = false; // cached on the CPU's side, for reading back
};

struct VulkanRenderer::Impl {
    VulkanRenderer &self;
    Memory &memory;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    // two frames in flight: what is recorded goes to the GPU without
    // waiting, the slot after is waited for before it is recorded into again
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        Buffer vertices, uniforms;
        uint64_t serial = 0; // which submission it holds
        std::vector<Buffer> staging;
        std::vector<struct Image> dead;
        bool pending = false;
    } slots[2];
    int current = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE; // the current slot's
    uint64_t submitted = 0;               // submissions so far; the next one is submitted + 1
    VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    VkPhysicalDeviceMemoryProperties memory_props{};
    PFN_vkCmdPushDescriptorSetKHR push_descriptors = nullptr;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, depth_module = VK_NULL_HANDLE;
    VkShaderModule frag_wbuffer = VK_NULL_HANDLE; // pica.frag with W_BUFFER
    VkShaderModule frag_shadow = VK_NULL_HANDLE;  // and with SHADOW
    bool shadow_pass = false;                     // a pass with no attachments, for the shadow output
    VkImageView words_view(Image &i);
    VkDescriptorSetLayout depth_set = VK_NULL_HANDLE;
    VkPipelineLayout depth_layout = VK_NULL_HANDLE;
    VkPipeline depth_pipeline = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE; // kept on disk with the translated shaders
    // the game's vertex programs as shaders of their own (translate.h);
    // RUNTIME_NO_TRANSLATE=1 keeps the interpreter
    std::unique_ptr<Translator> translator;
    std::unordered_map<uint32_t, VkSampler> samplers;

    Buffer vertices, uniforms;
    VkDeviceSize vertex_at = 0, uniform_at = 0;
    std::vector<Buffer> staging; // freed once the commands using them ran
    std::vector<Image> dead;     // the same for images let go

    std::vector<std::unique_ptr<Surface>> surfaces;
    // buffers another took the memory of while the frame's ops still draw
    // on them (a cutscene's passes, sized each their own, over the same
    // memory): kept for the frame's frames between, which draw them again,
    // until the ops go (drop_ops). Gone at once, their draws were left out
    // of the frames between, or drawn with no depth, and flashed
    std::vector<std::unique_ptr<Surface>> retired;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, Texture> textures;
    Image blank; // a unit with no texture

    bool recording = false;
    bool rendering = false;
    Surface *color_target = nullptr, *depth_target = nullptr;

    VertexStage stage;
    std::vector<Vertex> triangles;
    // the vertex shader on the GPU (RUNTIME_CPU_VERTICES=1: on the CPU, to
    // compare): the draw render takes, and with a frame between the frame
    // before's and where between them
    bool gpu_vertices = std::getenv("RUNTIME_CPU_VERTICES") == nullptr;
    VertexStage::Gathered gathered;
    GpuDraw live;
    const GpuDraw *gpu_draw = nullptr, *gpu_before = nullptr;
    float gpu_t = 1, gpu_jump = 1e9f;
    std::shared_ptr<const ProgramBlock> program_now;
    uint64_t program_version = ~0ull, program_hash = 0;
    VkDeviceSize program_offset = 0;
    uint64_t program_serial = ~0ull, program_sent = 0;
    VkShaderModule vert_gpu = VK_NULL_HANDLE;
    // the outputs of the game's draws, for the frames between: two halves,
    // a game frame each in turn (output_turn), so the frame before's stay
    static constexpr VkDeviceSize OUTPUT_HALF = 128u << 20;
    // a frame between takes its draws' positions one way for all: from the
    // outputs the game's draws kept, or each draw run again when any of them
    // has none (the room ran out, a pair's target not drawn the frame
    // before). The two ways differ in the last bits, and a character drawn
    // twice (a base, its lighting over it where the depths are equal) showed
    // the base through, dark, for a frame
    bool outputs_full = false; // the room ran out this frame
    bool run_all = false;      // the frame between being drawn runs every draw
    VkBuffer outputs = VK_NULL_HANDLE;
    VkDeviceMemory outputs_memory = VK_NULL_HANDLE;
    VkDeviceSize output_at = 0;
    uint64_t output_turn = 1;
    bool store_outputs = false; // the device lets vertex shaders write
    // the GPU's own time: a timestamp at each command buffer's start and end,
    // and a pair around each part (gpu_open, gpu_close): the passes that
    // draw, the frames between, their buffers' copies, the screens'
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double timestamp_ns = 0; // nanoseconds a tick
    static constexpr uint32_t QUERIES = 4096; // a slot's
    std::vector<std::pair<int, uint32_t>> scopes[2];
    uint32_t queries_used[2]{};
    int pass_scope = -1;
    // why passes end, for the counters: 0 another target, 1 a texture that
    // is the target, 2 the rest (copies, uploads, submits)
    int end_cause = 2;
    int gpu_open(int part);
    void gpu_close(int at);

    // the fragment lighting: what was last decoded, and where in the
    // uniform ring it and the tables went, for the submission they went with
    pica::LightingDecoder lighting;
    pica::LightingBlock light_block{};
    VkDeviceSize light_offset = 0, tables_offset = 0;
    uint64_t light_serial = ~0ull, tables_serial = ~0ull;
    // the tables as the fragment shader reads them: lighting's, the fog's
    // 128 (factor, step), then the procedural texture's: noise, color map
    // and alpha map (value, step), its colors and their steps (RGBA, 0-255)
    struct DecodedTables {
        pica::LightTables light;
        std::array<std::array<float, 2>, 128> fog;
        std::array<std::array<std::array<float, 2>, 128>, 3> maps;
        std::array<std::array<float, 4>, 256> colors, steps;
    } light_tables{};
    uint64_t tables_sent = ~0ull, tables_decoded = ~0ull; // Pica::tables_version of each

    // -- frames between the game's ----------------------------------------------
    // each game frame's draws, fills and transfers are kept; when the frame
    // ends (its display transfer into a top screen buffer, frame_end) they
    // are drawn again into each buffer's `between` image, every vertex
    // placed halfway between where the frame before and this one put it.
    // the next vertical blank shows that picture, the one after the game's
    // own (vblank). the game's logic is untouched
    int between = 0; // self.between, taken at each vertical blank
    bool replaying = false;
    const VkImageView *forced_views = nullptr;
    // in a replay, a buffer read as a texture is read from its between image
    std::unordered_map<VkImageView, VkImageView> between_views;
    Pica *last_pica = nullptr;
    using Tables = std::array<std::array<uint32_t, 256>, 32>;
    struct Op {
        enum Kind { Draw, Fill, Transfer } kind;
        std::vector<uint32_t> regs;
        std::vector<Vertex> vertices;
        std::array<VkImageView, 3> views;
        std::shared_ptr<const Tables> tables;
        uint64_t tables_version;
        std::array<uint32_t, 4> fill; // start, end, value, width
        Backend::Transfer transfer;
        uint64_t look = 0; // the textures and the combiners: two draws alike in them are the same thing
        uint64_t uvs = 0;  // where it samples its first texture: the same in a thing that only moves
        std::shared_ptr<GpuDraw> gpu; // shaded on the GPU: its inputs, not vertices
    };
    std::vector<Op> ops_now;
    std::map<uint32_t, std::vector<Op>> draws_before; // by target: the last frame's that drew it
    std::vector<Image> dead_held; // let go during a frame whose ops still use them
    std::shared_ptr<const Tables> tables_kept;
    uint64_t tables_kept_version = ~0ull;
    uint64_t blanks = 0, top_blank = 0; // vertical blanks so far, and at the last frame for the top screen
    uint64_t top_frames = 0, top_frames_shown = 0;
    uint32_t top_output = 0, top_shown = 0; // the buffer the last frame went to, the one the top screen showed
    // the buffer drawn into that the last top screen's frame came from, and
    // its rows (games draw both screens into one, each in rows of its own)
    uint32_t top_source = 0, top_rows[2] = {0, 0};
    bool drawn_flat(const Pica &p) const;   // the draw about to go: 2D (draw.cpp)
    std::unordered_set<uint64_t> flat_kinds; // draws (program, first texture) found 2D: stay so (draw.cpp)
    bool edge_filter(const Pica &p) const;  // and an outline's edge filter
    // the resolution the buffers are drawn at (VulkanRenderer::scale): a
    // PICA pixel is res of the image's each way
    float res = 1;
    uint32_t scaled(uint32_t n) const { return std::max(1u, uint32_t(std::lround(n * res))); }
    int32_t at_res(float n) const { return int32_t(std::lround(n * res)); }
    void rescale(float to);
    // the newest picture in each buffer (a counter of the transfers and
    // writes into it), and the buffer each screen showed
    std::unordered_map<uint32_t, uint64_t> picture_in;
    uint64_t pictures = 0;
    uint32_t showing[2] = {};
    bool new_before = false;
    bool between_ready = false; // the last frame was drawn between, into the buffer at between_address
    uint32_t between_address = 0;
    // the top screen's buffer as each frame between drew it, in order
    // (frame_end): how many, over how many blanks, and how many of them the
    // blanks since the frame showed have shown (vblank, `between` a blank).
    // two lists, a frame each in turn: the next frame often ends before the
    // last of this one's are shown
    std::vector<Image> between_pictures[2];
    int between_list = 0, between_count = 0, between_blanks = 0;
    int sequence = 0, sequence_list = 0, sequence_at = 0;

    void note_draw(Pica &p, const VkImageView *views);

    static uint32_t op_target(const Op &op);

    // each of the frame's draws paired with the one of the frame before it
    // is, among those to the same target made of the same (vertex count or
    // inputs, textures, combiners): the frame's pairs as a whole, the
    // likeliest first (between.cpp). A port can tell what a draw is: tagged
    // in a PICA register nothing uses (DRAW_TAG, a model's mesh), two meshes
    // never pair. Written into the game's command list it broke a scene
    // (Solstice's Poké Pelago drew nothing), so no port tags now
    std::vector<const Op *> pairs; // by place in ops_now, null for none or a jump
    std::vector<float> limits;     // by place: how far a vertex of the pair may move (jump_limit)
    void pair_frame();

    static float moved(const std::vector<Vertex> &before, const std::vector<Vertex> &now);

    // a draw shaded on the GPU: its positions at a few of its vertices, run
    // on the CPU
    static std::vector<Vertex> sample_positions(const GpuDraw &d);

    static bool jumps(const Op &before, const Op &now);

    static float jump_limit(const std::vector<Vertex> &before, const std::vector<Vertex> &now);

    void draw_between(float t);

    bool keep_between(uint32_t output, int slot);



    void frame_end(uint32_t output);

    void drop_ops();

    void let_go(const Image &i);

    // -- the screens, a frame each vertical blank ------------------------------
    // what each screen shows is copied at a blank and read at the next, when
    // the GPU is long done with it, then handed on as a Frame
    // a picture each of the parts of a blank: `between` of them (120 Hz
    // and up), PARTS at most
    static constexpr int PARTS = 4;
    struct Shown {
        Buffer buffers[2];
        uint32_t image_size[2][2]; // the images copied into buffers, as large as they are
        uint32_t size[2][2]{};
        bool on_gpu[2]{};
        Picture from_memory[2];
        uint64_t number = 0, serial = 0;
        bool between = false, pending = false, to_cpu = true;
        int set = -1, part = 0;
    } shown[PARTS];

    Surface *framebuffer(uint32_t address, uint32_t stride);

    bool vblank(Pica &p);

    void finish_shown(Pica &p);
    void finish_shown(Pica &p, Shown &sh);

    // RUNTIME_VK_DEBUG, read once
    const bool debug = std::getenv("RUNTIME_VK_DEBUG") != nullptr;

    Impl(VulkanRenderer &s, Memory &m, const std::vector<std::string> &extensions)
        : self(s), memory(m), instance_extensions(extensions) {}
    std::vector<std::string> instance_extensions;
    bool can_present = false; // the device has the swapchain

    // the screens copied on the GPU for a presenter (VulkanPresenter), a set
    // a vertical blank in turn; one is used again once the presenter's
    // timeline (presented) got past what it waited for
    static constexpr int SETS = 16;
    struct ScreenSet {
        Image screens[2];
        std::atomic<uint64_t> wait{0};
    } sets[SETS];
    int next_set = 0;
    // the submissions done (rendered, the serial of each) and the
    // presenter's (presented): timeline semaphores
    VkSemaphore rendered = VK_NULL_HANDLE, presented = VK_NULL_HANDLE;
    std::mutex queue_lock; // the queue: this thread's submits and the presenter's

    bool fail(const char *what, VkResult r);

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);

    // -- set up -------------------------------------------------------------

    bool init();

    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool read = false);

    // a staging buffer: a spare one that fits, else a new one (sizes in
    // powers of two, so they fit again)
    std::vector<Buffer> spare;
    Buffer staging_buffer(VkDeviceSize size, bool read);
    void give_back(Buffer &b);

    void destroy(Buffer &b);

    Image image(uint32_t w, uint32_t h, VkFormat format, bool depth);

    void destroy(Image &i);

    // -- recording and submitting -------------------------------------------

    void begin();

    void barrier();

    void end_rendering();

    void retire(Slot &s);

    void submit(bool wait = true);

    VkDeviceSize take(Buffer &ring, VkDeviceSize &at, VkDeviceSize size, VkDeviceSize align);

    // -- moving pixels between memory and images ------------------------------

    void copy_in(Image &i, const void *bytes, VkDeviceSize size, VkImageAspectFlags aspect);

    void upload_color(Image &i, const uint8_t *rgba, VkDeviceSize size);

    void upload(Surface &s);

    // an image's pixels back into the buffer's memory
    int cause = 0; // why the next write-back happens, for the counters
    void write_back(Surface &s);

    // -- buffers and textures ---------------------------------------------------

    static bool overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

    void sync(uint32_t paddr, uint32_t size, bool write);

    Surface *surface(uint32_t address, uint32_t w, uint32_t h, uint32_t format, bool depth, bool linear = false);

    void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width);

    bool transfer(const Backend::Transfer &t);

    VkSampler sampler(uint32_t params);

    VkImageView texture(Pica &p, int unit);
    VkImageView texture_at(Pica &p, int unit, uint32_t address);
    VkImageView cube_face(Pica &p, int face);

    VkImageView depth_as_color(Surface &s);

    // -- pipelines ---------------------------------------------------------------

    VkPipeline pipeline(uint64_t key, bool has_depth);
    VkPipeline make_pipeline(uint64_t key, bool has_depth, VkShaderModule vertex, const uint32_t *shape = nullptr) const;
    // the shape of a draw's state for the fragment shader (pica.frag): a
    // pipeline made for it leaves out what it does not use (RUNTIME_GENERIC_FRAG=1:
    // the one shader for all, to compare)
    static Shape shape_of(const State &st, const pica::LightingBlock &lit);
    const bool specialize = std::getenv("RUNTIME_GENERIC_FRAG") == nullptr;

    // -- drawing -----------------------------------------------------------------

    void render(Pica &p);
    void gpu_unit(Pica &p, GpuDraw &d);

    void guard(Surface &s);

    void shutdown();
    std::filesystem::path cache_folder;
};

} // namespace runtime

#endif
