// The Vulkan renderer: the PICA's draws, its buffers as images. One device,
// one queue and one command buffer, recorded as the command lists come and
// submitted when something needs what it drew (docs/vulkan-renderer.md).

#include "impl.h"

#include <cctype>

namespace runtime {

VulkanRenderer::VulkanRenderer(Memory &memory, const std::vector<std::string> &instance_extensions)
    : impl_(std::make_unique<Impl>(*this, memory, instance_extensions)) {
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
    impl_->stop_preloading = true;
    for (auto &t : impl_->preloaders) t.join();
    impl_->shutdown();
}

bool VulkanRenderer::ready() const { return impl_->device != VK_NULL_HANDLE; }

void VulkanRenderer::translation_counts(uint64_t &compiled, uint64_t &loaded, uint64_t &failed) const {
    compiled = loaded = failed = 0;
    if (!impl_->translator) return;
    compiled = impl_->translator->compiled, loaded = impl_->translator->loaded, failed = impl_->translator->failed;
}

void VulkanRenderer::textures_from(const std::vector<std::filesystem::path> &folders, LoadImage load, bool preload) {
    if (!ready()) return;
    Impl &d = *impl_;
    d.load_image = std::move(load);
    // each folder a pack of its own (its pack.json, or a legacy pack's
    // rules), the first to give a hash a file keeping it
    for (const std::filesystem::path &folder : folders) {
        if (size_t taken = d.packs.add_folder(folder))
            std::fprintf(stderr, "runtime: %zu textures of our own in %s\n", taken, folder.string().c_str());
    }
    if (preload && !d.packs.empty()) d.preload_replacements();
}

void VulkanRenderer::dump_textures(const std::filesystem::path &folder, SaveImage save) {
    if (!ready()) return;
    std::error_code e;
    std::filesystem::create_directories(folder, e);
    // a pack as it is: Azahar's pack.json beside the dumps (the new hash)
    write_pack_template(folder);
    impl_->dump_folder = folder;
    impl_->save_image = std::move(save);
}

void VulkanRenderer::cache_in(const std::string &folder) {
    if (!ready()) return;
    Impl &d = *impl_;
    d.cache_folder = std::filesystem::u8path(folder);
    if (d.translator) d.translator->cache_in(d.cache_folder);
    // the driver's pipelines from the last run, merged into the cache
    if (FILE *f = open_file(d.cache_folder / "pipelines.bin", "rb")) {
        std::vector<uint8_t> data;
        uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) data.insert(data.end(), chunk, chunk + n);
        std::fclose(f);
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.data();
        VkPipelineCache old = VK_NULL_HANDLE;
        if (vkCreatePipelineCache(d.device, &ci, nullptr, &old) == VK_SUCCESS) {
            vkMergePipelineCaches(d.device, d.pipeline_cache, 1, &old);
            vkDestroyPipelineCache(d.device, old, nullptr);
        }
    }
}

namespace {
using clock = std::chrono::steady_clock;
double since(clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); }
} // namespace

void VulkanRenderer::draw(Pica &p, bool indexed) {
    if (!ready()) return;
    Impl &d = *impl_;
    auto t = clock::now();
    const auto &r = p.regs;
    const uint32_t fog = r[0x0E0] & 7, operation = r[COLOR_OPERATION] & 3;
    stats.lacking[0] += fog == 7 || operation == 1;
    stats.lacking[1] += (r[GEOSTAGE_CONFIG] & 3) == 2 && (r[GS_CONFIG] & 0xFF) > 2;
    // the inputs gathered here and the shader run on the GPU, or all of it here
    if (d.gpu_vertices && d.stage.gather(p, indexed, d.gathered)) {
        GpuDraw &live = d.live;
        std::swap(live.inputs, d.gathered.inputs);
        std::swap(live.indices, d.gathered.indices);
        live.stride = d.gathered.stride;
        d.gpu_unit(p, live);
        stats.time_vertices += since(t);
        t = clock::now();
        d.gpu_draw = &live;
        d.render(p);
        d.gpu_draw = nullptr;
        stats.time_render += since(t);
        return;
    }
    d.stage.draw(p, indexed, d.triangles);
    stats.time_vertices += since(t);
    t = clock::now();
    d.render(p);
    stats.time_render += since(t);
}

void VulkanRenderer::draw_immediate(Pica &p, const std::vector<std::array<Vec4, 16>> &vertices) {
    if (!ready()) return;
    impl_->stage.draw_immediate(p, vertices, impl_->triangles);
    impl_->render(p);
}

void VulkanRenderer::fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    auto t = clock::now();
    if (ready()) impl_->cause = 2, impl_->fill(start, end, value, width);
    stats.time_sync += since(t);
}

bool VulkanRenderer::vblank(Pica &p) { return ready() && impl_->vblank(p); }

bool VulkanRenderer::display_transfer(const Transfer &transfer) {
    auto t = clock::now();
    bool done = ready() && impl_->transfer(transfer);
    stats.time_sync += since(t);
    return done;
}

void VulkanRenderer::sync(uint32_t paddr, uint32_t size, bool write) {
    auto t = clock::now();
    if (ready()) impl_->cause = 1, impl_->sync(paddr, size, write);
    stats.time_sync += since(t);
}

} // namespace runtime
