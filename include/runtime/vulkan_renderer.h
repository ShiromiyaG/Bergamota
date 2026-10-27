// The runtime's renderer on the host GPU through Vulkan: the PICA's draws
// as Vulkan draws, its buffers kept as images until something else needs
// them in memory (docs/vulkan-renderer.md).
#ifndef RUNTIME_VULKAN_RENDERER_H
#define RUNTIME_VULKAN_RENDERER_H

#include "runtime/pica.h"

#include <memory>
#include <string>

namespace runtime {

// the PICA's logic op (GPUREG_LOGIC_OP, in Zakuro's blend.rs order) as a
// VkLogicOp
int vk_logic_op(uint32_t pica_op);

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

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime

#endif
