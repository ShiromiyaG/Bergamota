// The runtime's renderer on the host GPU through Vulkan: the PICA's draws
// as Vulkan draws, its buffers kept as images until something else needs
// them in memory (docs/vulkan-renderer.md).
#ifndef RUNTIME_VULKAN_RENDERER_H
#define RUNTIME_VULKAN_RENDERER_H

#include "runtime/pica.h"

#include <vulkan/vulkan.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace runtime {

// a window the renderer shows the screens in itself, straight from the
// GPU: what the window system needs of the instance, and the surface made
// on it (SDL_Vulkan_GetInstanceExtensions, SDL_Vulkan_CreateSurface)
struct PresentTarget {
    std::vector<const char *> instance_extensions;
    std::function<VkSurfaceKHR(VkInstance)> make_surface;
};

// a screen shown: where the game keeps it (as the LCD is set up), and the
// rectangle it goes to, in the window's pixels
struct ScreenView {
    uint32_t address, stride, format;
    float x, y, width, height;
};

// what Dear ImGui's Vulkan backend needs to draw over the screens
struct VulkanHandles {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkFormat format; // the swapchain's
    uint32_t images; // in the swapchain
};

// the PICA's logic op (GPUREG_LOGIC_OP, in Zakuro's blend.rs order) as a
// VkLogicOp
int vk_logic_op(uint32_t pica_op);

// the PICA's scissor test (GPUREG_SCISSORTEST_MODE, _POS1, _POS2: corners
// inclusive, rows up from the bottom) as a rectangle of an image of a w by
// h buffer at a scale, rows down, its ends exclusive; mode: 0 off, 1 the
// outside of it drawn, 3 the inside
struct ScissorBox {
    uint32_t mode, x0, y0, x1, y1;
};
ScissorBox pica_scissor(uint32_t mode, uint32_t low, uint32_t high, uint32_t w, uint32_t h, uint32_t scale);

class VulkanRenderer : public pica::Backend {
public:
    // pipeline_cache: a file the compiled pipelines are kept in between runs
    // (none: they are compiled anew each run); target: a window to show the
    // screens in (none: they are only read back, picture()); scale: the
    // game's buffers drawn at that many times their size (1-4), memory
    // getting them at their own
    explicit VulkanRenderer(Memory &memory, std::string pipeline_cache = {}, const PresentTarget *target = nullptr,
                            uint32_t scale = 1);
    ~VulkanRenderer() override;
    // false when there is no Vulkan 1.3 device; error says why
    bool ready() const;
    std::string error;

    // whether it shows the screens in the window itself (a target was given
    // and its swapchain could be made)
    bool presents() const;
    VulkanHandles handles() const;
    // ends the frame: the screens drawn into the window from the surfaces
    // the game drew them in (from memory when it did not), smooth or in
    // sharp pixels, overlay drawing over them, and shown; waiting for the
    // display with vsync. false when nothing could be shown (the window
    // minimised): the frame was still sent
    bool present(const ScreenView *screens, int count, bool smooth, bool vsync,
                 const std::function<void(VkCommandBuffer)> &overlay);

    void draw(pica::Pica &pica, bool indexed) override;
    void draw_immediate(pica::Pica &pica, const std::vector<std::array<pica::Vec4, 16>> &vertices) override;
    void sync(uint32_t paddr, uint32_t size, bool write) override;
    void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) override;
    bool display_transfer(const Transfer &transfer) override;
    bool picture(uint32_t screen, uint32_t address, uint32_t stride, uint32_t format, Picture &out) override;
    void frame_done() override;

    // counters
    uint64_t draws = 0, triangles = 0, uploads = 0, write_backs = 0, submits = 0;
    // draws not drawn: the vertex stage cannot do them (a geometry shader
    // mode other than 0), or the device has no fragment atomics for the
    // shadow mode, or its buffer is not RGBA8
    uint64_t refused = 0;
    // pipelines made this run, and the seconds the driver took making them
    uint64_t pipelines = 0;
    double time_pipelines = 0;
    // where the time went, in seconds: the vertex stage, recording draws, syncs
    double time_vertices = 0, time_render = 0, time_sync = 0;
    // write-backs by cause: the CPU (guards), the front end, a fill, a texture
    uint64_t write_back_causes[4]{};

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime

#endif
