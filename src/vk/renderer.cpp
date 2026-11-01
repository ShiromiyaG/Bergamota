// The Vulkan renderer: the PICA's draws, its buffers as images. One device,
// one queue and one command buffer, recorded as the command lists come and
// submitted when something needs what it drew (docs/vulkan-renderer.md).

#include "impl.h"

namespace runtime {

VulkanRenderer::VulkanRenderer(Memory &memory) : impl_(std::make_unique<Impl>(*this, memory)) {
    if (!impl_->init()) return;
    // the CPU's accesses to what was drawn bring it down first (memory's guards)
    memory.gpu_sync = [this, &memory](uint32_t vaddr, uint32_t size, bool write) {
        uint32_t paddr = vaddr >= map::VRAM_VADDR && vaddr < map::VRAM_VADDR + map::VRAM_SIZE
                             ? map::VRAM_PADDR + (vaddr - map::VRAM_VADDR)
                             : map::FCRAM_PADDR + (vaddr - memory.linear_base);
        impl_->cause = 0;
        impl_->sync(paddr, size, write);
    };
}

VulkanRenderer::~VulkanRenderer() {
    impl_->memory.gpu_sync = nullptr;
    impl_->shutdown();
}

bool VulkanRenderer::ready() const { return impl_->device != VK_NULL_HANDLE; }

namespace {
using clock = std::chrono::steady_clock;
double since(clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); }
} // namespace

void VulkanRenderer::draw(Pica &p, bool indexed) {
    if (!ready()) return;
    auto t = clock::now();
    impl_->stage.draw(p, indexed, impl_->triangles);
    time_vertices += since(t);
    t = clock::now();
    impl_->render(p);
    time_render += since(t);
}

void VulkanRenderer::draw_immediate(Pica &p, const std::vector<std::array<Vec4, 16>> &vertices) {
    if (!ready()) return;
    impl_->stage.draw_immediate(p, vertices, impl_->triangles);
    impl_->render(p);
}

void VulkanRenderer::fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    auto t = clock::now();
    if (ready()) impl_->cause = 2, impl_->fill(start, end, value, width);
    time_sync += since(t);
}

bool VulkanRenderer::vblank(Pica &p) { return ready() && impl_->vblank(p); }

bool VulkanRenderer::display_transfer(const Transfer &transfer) {
    auto t = clock::now();
    bool done = ready() && impl_->transfer(transfer);
    time_sync += since(t);
    return done;
}

void VulkanRenderer::sync(uint32_t paddr, uint32_t size, bool write) {
    auto t = clock::now();
    if (ready()) impl_->cause = 1, impl_->sync(paddr, size, write);
    time_sync += since(t);
}

} // namespace runtime
