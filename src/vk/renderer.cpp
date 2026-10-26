// The Vulkan renderer: the PICA's draws, its buffers as images. One device,
// one queue and one command buffer, recorded as the command lists come and
// submitted when something needs what it drew (docs/vulkan-renderer.md).

#include "runtime/vulkan_renderer.h"
#include "runtime/memory.h"

#include "../pica/texture.h"
#include "../pica/vertex.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace runtime {

using namespace pica;

namespace {

const uint32_t VERT_SPV[] = {
#include "pica.vert.inc"
};
const uint32_t FRAG_SPV[] = {
#include "pica.frag.inc"
};
const uint32_t DEPTH_SPV[] = {
#include "depth.comp.inc"
};

constexpr VkDeviceSize VERTEX_RING = 64u << 20, UNIFORM_RING = 8u << 20;

// the registers the shaders read, by slot (pica.frag)
struct State {
    uint32_t regs[64];
    float depth[4];
};
constexpr uint32_t STATE_REGS[] = {
    0x0C0, 0x0C1, 0x0C2, 0x0C3, 0x0C4, 0x0C8, 0x0C9, 0x0CA, 0x0CB, 0x0CC, 0x0D0, 0x0D1, 0x0D2, 0x0D3, 0x0D4,
    0x0D8, 0x0D9, 0x0DA, 0x0DB, 0x0DC, 0x0F0, 0x0F1, 0x0F2, 0x0F3, 0x0F4, 0x0F8, 0x0F9, 0x0FA, 0x0FB, 0x0FC,
    0x0E0, 0x0FD, 0x104, 0x080, 0x083, 0x093, 0x09B, 0x081, 0x091, 0x099,
};

// a texture unit's registers: border color, then size, parameters, address
// and format
constexpr uint32_t UNIT_BASE[3] = {0x081, 0x091, 0x099};
uint32_t unit_format(const Pica &p, int unit) { return p.regs[unit ? UNIT_BASE[unit] + 5 : 0x08E] & 15; }

// four lanes side by side, which the CPU runs at memory speed
uint64_t hash_bytes(const uint8_t *p, size_t n) {
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

} // namespace

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE, stencil = VK_NULL_HANDLE; // a depth image's aspects, to sample
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
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    VkPhysicalDeviceMemoryProperties memory_props{};
    PFN_vkCmdPushDescriptorSetKHR push_descriptors = nullptr;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, depth_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_set = VK_NULL_HANDLE;
    VkPipelineLayout depth_layout = VK_NULL_HANDLE;
    VkPipeline depth_pipeline = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    std::unordered_map<uint32_t, VkSampler> samplers;

    Buffer vertices, uniforms;
    VkDeviceSize vertex_at = 0, uniform_at = 0;
    std::vector<Buffer> staging; // freed once the commands using them ran
    std::vector<Image> dead;     // the same for images let go

    std::vector<std::unique_ptr<Surface>> surfaces;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, Texture> textures;
    Image blank; // a unit with no texture

    bool recording = false;
    bool rendering = false;
    Surface *color_target = nullptr, *depth_target = nullptr;

    VertexStage stage;
    std::vector<Vertex> triangles;

    Impl(VulkanRenderer &s, Memory &m) : self(s), memory(m) {}

    bool fail(const char *what, VkResult r) {
        self.error = std::string(what) + " failed (" + std::to_string(int(r)) + ")";
        return false;
    }

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
            if ((bits >> i & 1) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        return 0;
    }

    // -- set up -------------------------------------------------------------

    bool init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "runtime";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        const char *validation = "VK_LAYER_KHRONOS_validation";
        if (std::getenv("RUNTIME_VK_VALIDATION")) ici.enabledLayerCount = 1, ici.ppEnabledLayerNames = &validation;
        if (VkResult r = vkCreateInstance(&ici, nullptr, &instance)) return fail("vkCreateInstance", r);

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> all(count);
        vkEnumeratePhysicalDevices(instance, &count, all.data());
        // a discrete GPU first, then any with Vulkan 1.3
        for (int pass = 0; pass < 2 && !physical; pass++) {
            for (VkPhysicalDevice d : all) {
                VkPhysicalDeviceProperties p;
                vkGetPhysicalDeviceProperties(d, &p);
                if (p.apiVersion < VK_API_VERSION_1_3) continue;
                if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
                physical = d;
                break;
            }
        }
        if (!physical) {
            self.error = "no GPU with Vulkan 1.3";
            return false;
        }
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fams(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fams.data());
        for (uint32_t i = 0; i < families; i++) {
            if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                break;
            }
        }
        // D24S8 where the GPU has it (AMD's do not), else D32 with stencil
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;

        float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        v13.dynamicRendering = VK_TRUE;
        v13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        eds.extendedDynamicState = VK_TRUE;
        v13.pNext = &eds;
        const char *extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &v13;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 2;
        dci.ppEnabledExtensionNames = extensions;
        if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device)) return fail("vkCreateDevice", r);
        vkGetDeviceQueue(device, family, 0, &queue);
        push_descriptors = PFN_vkCmdPushDescriptorSetKHR(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        if (!push_descriptors) {
            self.error = "no VK_KHR_push_descriptor";
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
            return false;
        }

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        vkCreateCommandPool(device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cai, &cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device, &fci, nullptr, &fence);

        vertices = buffer(VERTEX_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        uniforms = buffer(UNIFORM_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        // the one layout: the state, and the three textures
        VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sli.bindingCount = 2;
        sli.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &sli, nullptr, &set_layout);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        vkCreatePipelineLayout(device, &pli, nullptr, &layout);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sizeof VERT_SPV;
        smi.pCode = VERT_SPV;
        vkCreateShaderModule(device, &smi, nullptr, &vert);
        smi.codeSize = sizeof FRAG_SPV;
        smi.pCode = FRAG_SPV;
        vkCreateShaderModule(device, &smi, nullptr, &frag);

        // depth read as color (depth.comp)
        VkDescriptorSetLayoutBinding db[3] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        sli.bindingCount = 3;
        sli.pBindings = db;
        vkCreateDescriptorSetLayout(device, &sli, nullptr, &depth_set);
        pli.pSetLayouts = &depth_set;
        vkCreatePipelineLayout(device, &pli, nullptr, &depth_layout);
        smi.codeSize = sizeof DEPTH_SPV;
        smi.pCode = DEPTH_SPV;
        vkCreateShaderModule(device, &smi, nullptr, &depth_module);
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, depth_module, "main", nullptr};
        cpi.layout = depth_layout;
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &depth_pipeline);
        VkSamplerCreateInfo nsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        vkCreateSampler(device, &nsi, nullptr, &nearest);

        begin();
        blank = image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        uint8_t zero[4] = {};
        upload_color(blank, zero, 4);
        return true;
    }

    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        b.size = size;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        vkCreateBuffer(device, &bi, nullptr, &b.buffer);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &ai, nullptr, &b.memory);
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, size, 0, reinterpret_cast<void **>(&b.mapped));
        return b;
    }

    void destroy(Buffer &b) {
        if (!b.buffer) return;
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }

    // images stay in the general layout: attachment, sampled and copied alike
    Image image(uint32_t w, uint32_t h, VkFormat format, bool depth) {
        Image i;
        i.width = w, i.height = h;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT |
                   (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
        vkCreateImage(device, &ii, nullptr, &i.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ai, nullptr, &i.memory);
        vkBindImageMemory(device, i.image, i.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
        vkCreateImageView(device, &vi, nullptr, &i.view);
        if (depth) {
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            vkCreateImageView(device, &vi, nullptr, &i.depth);
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCreateImageView(device, &vi, nullptr, &i.stencil);
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        // into the general layout, once
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = i.image;
        b.subresourceRange = vi.subresourceRange;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        end_rendering();
        vkCmdPipelineBarrier2(cmd, &di);
        return i;
    }

    void destroy(Image &i) {
        if (!i.image) return;
        vkDestroyImageView(device, i.view, nullptr);
        if (i.depth) vkDestroyImageView(device, i.depth, nullptr), vkDestroyImageView(device, i.stencil, nullptr);
        vkDestroyImage(device, i.image, nullptr);
        vkFreeMemory(device, i.memory, nullptr);
        i = {};
    }

    // -- recording and submitting -------------------------------------------

    void begin() {
        if (recording) return;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        recording = true;
    }

    // everything waits for everything before: between passes, copies and
    // reads of what was drawn
    void barrier() {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        m.srcAccessMask = m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    void end_rendering() {
        if (!rendering) return;
        vkCmdEndRendering(cmd);
        rendering = false;
        color_target = depth_target = nullptr;
        barrier();
    }

    // runs what is recorded, and waits for it
    void submit() {
        begin();
        end_rendering();
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
        vkResetCommandBuffer(cmd, 0);
        recording = false;
        for (Buffer &b : staging) destroy(b);
        staging.clear();
        for (Image &i : dead) destroy(i);
        dead.clear();
        vertex_at = uniform_at = 0;
        self.submits++;
        begin();
    }

    // room in a ring, submitting what is there when it runs out
    VkDeviceSize take(Buffer &ring, VkDeviceSize &at, VkDeviceSize size, VkDeviceSize align) {
        at = (at + align - 1) / align * align;
        if (at + size > ring.size) submit();
        VkDeviceSize where = at;
        at += size;
        return where;
    }

    // -- moving pixels between memory and images ------------------------------

    void copy_in(Image &i, const void *bytes, VkDeviceSize size, VkImageAspectFlags aspect) {
        Buffer b = buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(b.mapped, bytes, size);
        end_rendering();
        VkBufferImageCopy c{};
        c.imageSubresource = {aspect, 0, 0, 1};
        c.imageExtent = {i.width, i.height, 1};
        vkCmdCopyBufferToImage(cmd, b.buffer, i.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier();
        staging.push_back(b);
        self.uploads++;
    }

    void upload_color(Image &i, const uint8_t *rgba, VkDeviceSize size) { copy_in(i, rgba, size, VK_IMAGE_ASPECT_COLOR_BIT); }

    // a buffer's memory into its image: rows as memory has them, 8x8 tiles
    void upload(Surface &s) {
        s.version++;
        const uint8_t *src = memory.host(s.address, s.bytes());
        if (!src) return;
        uint32_t w = s.width, h = s.height;
        if (!s.depth) {
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            uint32_t bpp = bytes_per_pixel(s.format);
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    auto c = decode_color(s.format, src + s.offset(x, y, bpp));
                    std::memcpy(&rgba[(size_t(y) * w + x) * 4], c.data(), 4);
                }
            }
            upload_color(s.image, rgba.data(), rgba.size());
            return;
        }
        // depth (16 or 24 bits) and stencil (the top byte of D24S8)
        static constexpr uint32_t BYTES[4] = {2, 2, 3, 4};
        uint32_t bpp = BYTES[s.format & 3];
        std::vector<uint32_t> depth(size_t(w) * h);
        std::vector<uint8_t> stencil(size_t(w) * h);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = src + s.offset(x, y, bpp);
                uint32_t v = bpp == 2 ? uint32_t(p[0] | p[1] << 8) << 8 : uint32_t(p[0] | p[1] << 8 | p[2] << 16);
                size_t at = size_t(y) * w + x;
                depth[at] = depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ? std::bit_cast<uint32_t>(float(v) / 16777215.0f) : v;
                stencil[at] = bpp == 4 ? p[3] : 0;
            }
        }
        copy_in(s.image, depth.data(), depth.size() * 4, VK_IMAGE_ASPECT_DEPTH_BIT);
        copy_in(s.image, stencil.data(), stencil.size(), VK_IMAGE_ASPECT_STENCIL_BIT);
    }

    // an image's pixels back into the buffer's memory
    int cause = 0; // why the next write-back happens, for the counters
    void write_back(Surface &s) {
        self.write_back_causes[cause]++;
        uint32_t w = s.width, h = s.height;
        VkDeviceSize texels = VkDeviceSize(w) * h;
        Buffer a = buffer(texels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT), b{};
        end_rendering();
        VkBufferImageCopy c{};
        c.imageSubresource = {VkImageAspectFlags(s.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, s.image.image, VK_IMAGE_LAYOUT_GENERAL, a.buffer, 1, &c);
        if (s.depth) {
            b = buffer(texels, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCmdCopyImageToBuffer(cmd, s.image.image, VK_IMAGE_LAYOUT_GENERAL, b.buffer, 1, &c);
        }
        barrier();
        submit();
        uint8_t *dst = memory.host(s.address, s.bytes());
        if (dst) {
            if (!s.depth) {
                uint32_t bpp = bytes_per_pixel(s.format);
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < w; x++) {
                        const uint8_t *p = a.mapped + (size_t(y) * w + x) * 4;
                        encode_color(s.format, {p[0], p[1], p[2], p[3]}, dst + s.offset(x, y, bpp));
                    }
                }
            } else {
                static constexpr uint32_t BYTES[4] = {2, 2, 3, 4};
                uint32_t bpp = BYTES[s.format & 3];
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < w; x++) {
                        size_t at = size_t(y) * w + x;
                        uint32_t v;
                        std::memcpy(&v, a.mapped + at * 4, 4);
                        if (depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT) v = uint32_t(std::clamp(std::bit_cast<float>(v), 0.0f, 1.0f) * 16777215.0f + 0.5f);
                        v &= 0xFFFFFF;
                        uint8_t *p = dst + s.offset(x, y, bpp);
                        if (bpp == 2) v >>= 8, p[0] = uint8_t(v), p[1] = uint8_t(v >> 8);
                        else p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16);
                        if (bpp == 4) p[3] = b.mapped[at];
                    }
                }
            }
        }
        destroy(a);
        destroy(b);
        s.dirty = false;
        self.write_backs++;
    }

    // -- buffers and textures ---------------------------------------------------

    static bool overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

    // memory over a range is about to be read or written by something else
    void sync(uint32_t paddr, uint32_t size, bool write) {
        for (auto &s : surfaces) {
            if (!overlaps(s->address, s->bytes(), paddr, size)) continue;
            if (s->dirty) write_back(*s);
            if (write) { // memory has the newer one from now on
                if (color_target == s.get() || depth_target == s.get()) end_rendering();
                s->stale = true;
            }
        }
    }

    Surface *surface(uint32_t address, uint32_t w, uint32_t h, uint32_t format, bool depth, bool linear = false) {
        for (auto &s : surfaces) {
            if (s->address == address && s->width == w && s->height == h && s->format == format && s->depth == depth &&
                s->linear == linear) {
                if (s->stale) {
                    if (color_target == s.get() || depth_target == s.get()) end_rendering();
                    upload(*s);
                    s->stale = false;
                }
                return s.get();
            }
        }
        // others over the same memory go back first, then away
        Surface fresh{address, w, h, format, depth, linear, {}};
        for (size_t i = 0; i < surfaces.size();) {
            Surface &s = *surfaces[i];
            if (!overlaps(s.address, s.bytes(), address, fresh.bytes())) {
                i++;
                continue;
            }
            if (s.dirty) write_back(s);
            if (color_target == &s || depth_target == &s) end_rendering();
            dead.push_back(s.image);
            if (s.as_color.image) dead.push_back(s.as_color);
            surfaces.erase(surfaces.begin() + i);
        }
        auto s = std::make_unique<Surface>(fresh);
        s->image = image(w, h, depth ? depth_format : VK_FORMAT_R8G8B8A8_UNORM, depth);
        upload(*s);
        surfaces.push_back(std::move(s));
        return surfaces.back().get();
    }

    // a fill: a buffer it covers whole is cleared on the GPU, the rest goes
    // back to memory first and loads again after
    void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
        for (auto &s : surfaces) {
            if (!overlaps(s->address, s->bytes(), start, end - start)) continue;
            bool whole = start <= s->address && end >= s->address + s->bytes() && !s->linear;
            if (!whole) {
                if (s->dirty) write_back(*s);
                if (color_target == s.get() || depth_target == s.get()) end_rendering();
                s->stale = true;
                continue;
            }
            end_rendering();
            uint8_t pattern[4];
            std::memcpy(pattern, &value, 4);
            VkImageSubresourceRange range{VkImageAspectFlags(s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
            if (s->depth) {
                // D16 or D24 in the low bytes, the stencil above
                uint32_t d = width == 2 ? uint32_t(value & 0xFFFF) << 8 : value & 0xFFFFFF;
                VkClearDepthStencilValue v{float(d) / 16777215.0f, width == 4 ? value >> 24 : 0};
                vkCmdClearDepthStencilImage(cmd, s->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            } else {
                // the pattern is the buffer's own pixel
                auto c = bytes_per_pixel(s->format) == width ? decode_color(s->format, pattern) : decode_color(0, pattern);
                VkClearColorValue v{{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f}};
                vkCmdClearColorImage(cmd, s->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            barrier();
            s->dirty = false; // memory gets the same fill
            s->version++;
            s->stale = false;
        }
    }

    // a display transfer from a buffer the GPU drew, into another of its
    // own: a blit flips and halves (averaging) on the way
    bool transfer(const Backend::Transfer &t) {
        // the input can start some rows into a buffer (a band of 8-row tiles):
        // games draw both screens into one and transfer each from its rows
        Surface *in = nullptr;
        uint32_t row = 0;
        for (auto &s : surfaces) {
            if (s->depth || s->stale || s->linear || s->width != t.in_width || t.input < s->address) continue;
            uint32_t band = s->width * 8 * bytes_per_pixel(s->format), offset = t.input - s->address;
            if (offset % band || offset / band * 8 + t.copy_height * t.scale_y > s->height) continue;
            in = s.get();
            row = offset / band * 8;
        }
        if (!in || !in->dirty) {
            static int shown = 0;
            if (std::getenv("RUNTIME_VK_DEBUG") && shown++ < 12) {
                std::fprintf(stderr, "transfer %08X %ux%u -> %08X %ux%u fmt %u tiled %d:", t.input, t.in_width, t.in_height, t.output,
                             t.out_width, t.out_height, t.out_format, t.out_tiled);
                for (auto &s : surfaces) {
                    if (overlaps(s->address, s->bytes(), t.input, t.in_width * t.in_height * 4))
                        std::fprintf(stderr, " [%08X %ux%u f%u d%d dirty%d stale%d lin%d]", s->address, s->width, s->height, s->format, s->depth, s->dirty, s->stale, s->linear);
                }
                std::fprintf(stderr, "\n");
            }
            return false;
        }
        Surface *out = surface(t.output, t.out_width, t.out_height, t.out_format, false, !t.out_tiled);
        if (out == in) return false;
        end_rendering();
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[0] = {0, int32_t(row), 0};
        b.srcOffsets[1] = {int32_t(t.copy_width * t.scale_x), int32_t(row + t.copy_height * t.scale_y), 1};
        if (t.flip) {
            b.dstOffsets[0] = {0, int32_t(t.out_height), 0};
            b.dstOffsets[1] = {int32_t(t.copy_width), int32_t(t.out_height - t.copy_height), 1};
        } else {
            b.dstOffsets[1] = {int32_t(t.copy_width), int32_t(t.copy_height), 1};
        }
        bool scaled = t.scale_x > 1 || t.scale_y > 1;
        vkCmdBlitImage(cmd, in->image.image, VK_IMAGE_LAYOUT_GENERAL, out->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        barrier();
        out->dirty = true;
        guard(*out);
        return true;
    }

    VkSampler sampler(uint32_t params) {
        // filters in bits 1-2, the wraps for t and s in 8-10 and 12-14
        uint32_t key = params & 0x7706;
        auto it = samplers.find(key);
        if (it != samplers.end()) return it->second;
        auto wrap = [](uint32_t w) {
            switch (w & 7) {
            case 2: case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // the border is the shader's
            }
        };
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = params & 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = params & 4 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.addressModeU = wrap(params >> 12);
        si.addressModeV = wrap(params >> 8);
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        VkSampler s;
        vkCreateSampler(device, &si, nullptr, &s);
        samplers[key] = s;
        return s;
    }

    // what a unit samples: a buffer drawn there, or the texture in memory
    VkImageView texture(Pica &p, int unit) {
        if (!(p.regs[TEXTURE_CONFIG] >> unit & 1)) return blank.view;
        uint32_t base = UNIT_BASE[unit], dims = p.regs[base + 1];
        uint32_t h = dims & 0x7FF, w = (dims >> 16) & 0x7FF, format = unit_format(p, unit);
        uint32_t address = (p.regs[base + 4] & 0x0FFFFFFF) << 3;
        if (!w || !h || !address) return blank.view;
        // a color buffer drawn into, read in place when it is the same
        for (auto &s : surfaces) {
            if (!s->depth && !s->linear && !s->stale && s->address == address && s->width == w && s->height == h && format <= 4 &&
                s->format == format) {
                if (color_target == s.get()) end_rendering();
                return s->image.view;
            }
        }
        // a D24S8 buffer read as RGBA8: made on the GPU
        for (auto &s : surfaces) {
            if (s->depth && !s->stale && s->format == 3 && format == 0 && s->address == address && s->width == w && s->height == h) {
                return depth_as_color(*s);
            }
        }
        size_t size = size_t(w) * h * texture_bits(format) / 8;
        if (std::getenv("RUNTIME_VK_DEBUG")) {
            static int shown = 0;
            for (auto &s : surfaces) {
                if (s->dirty && overlaps(s->address, s->bytes(), address, uint32_t(size)) && shown++ < 10)
                    std::fprintf(stderr, "texture %08X %ux%u fmt %u over surface %08X %ux%u fmt %u depth %d\n", address, w, h, format,
                                 s->address, s->width, s->height, s->format, s->depth);
            }
        }
        cause = 3;
        sync(address, uint32_t(size), false);
        const uint8_t *bytes = memory.host(address, uint32_t(size));
        if (!bytes) return blank.view;
        Texture &t = textures[{address, format, w, h}];
        // looked at again once a frame (a frame ends in display transfers), it
        // can have changed
        if (t.checked_list != p.transfers || !t.image.image) {
            t.checked_list = p.transfers;
            uint64_t hash = hash_bytes(bytes, size);
            if (hash != t.hash || !t.image.image) {
                if (!t.image.image) t.image = image(w, h, VK_FORMAT_R8G8B8A8_UNORM, false);
                std::vector<uint8_t> rgba(size_t(w) * h * 4);
                decode_texture(format, bytes, w, h, rgba.data());
                upload_color(t.image, rgba.data(), rgba.size());
                t.hash = hash;
            }
        }
        return t.image.view;
    }

    VkImageView depth_as_color(Surface &s) {
        if (!s.as_color.image) s.as_color = image(s.width, s.height, VK_FORMAT_R8G8B8A8_UNORM, false);
        if (s.converted == s.version) return s.as_color.view;
        end_rendering();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline);
        VkDescriptorImageInfo images[3] = {
            {nearest, s.image.depth, VK_IMAGE_LAYOUT_GENERAL},
            {nearest, s.image.stencil, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, s.as_color.view, VK_IMAGE_LAYOUT_GENERAL},
        };
        VkWriteDescriptorSet writes[3]{};
        for (int i = 0; i < 3; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstBinding = uint32_t(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        push_descriptors(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depth_layout, 0, 3, writes);
        vkCmdDispatch(cmd, (s.width + 7) / 8, (s.height + 7) / 8, 1);
        barrier();
        s.converted = s.version;
        return s.as_color.view;
    }

    // -- pipelines ---------------------------------------------------------------

    VkPipeline pipeline(uint64_t key, bool has_depth) {
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
        };
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[8] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, quaternion)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv0)},
            {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv1)},
            {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv2)},
            {6, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, w0)},
            {7, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, view)},
        };
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 8;
        vin.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        // the key: blend on (bit 0), the equations and factors, the color mask
        static constexpr VkBlendFactor FACTOR[16] = {
            VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
            VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
            VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE};
        static constexpr VkBlendOp EQUATION[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                                  VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        uint32_t func = uint32_t(key >> 8);
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = key & 1;
        blend.colorBlendOp = EQUATION[func & 7];
        blend.alphaBlendOp = EQUATION[(func >> 8) & 7];
        blend.srcColorBlendFactor = FACTOR[(func >> 16) & 15];
        blend.dstColorBlendFactor = FACTOR[(func >> 20) & 15];
        blend.srcAlphaBlendFactor = FACTOR[(func >> 24) & 15];
        blend.dstAlphaBlendFactor = FACTOR[(func >> 28) & 15];
        blend.colorWriteMask = (key >> 4) & 15;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dynamic[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        };
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = uint32_t(std::size(dynamic));
        dyn.pDynamicStates = dynamic;
        VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &color_format;
        ri.depthAttachmentFormat = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
        ri.stencilAttachmentFormat = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &ri;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vin;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &ds;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &dyn;
        gi.layout = layout;
        VkPipeline pipe = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe);
        pipelines[key] = pipe;
        return pipe;
    }

    // -- drawing -----------------------------------------------------------------

    void render(Pica &p) {
        const auto &r = p.regs;
        self.draws++;
        if (triangles.empty()) return;
        uint32_t dims = r[FRAMEBUFFER_DIMENSIONS], w = dims & 0x7FF, h = ((dims >> 12) & 0x3FF) + 1;
        uint32_t color_address = (r[COLOR_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3;
        uint32_t depth_address = (r[DEPTH_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3;
        if (!color_address || !w || w > 1024 || h > 1024) return;
        uint32_t mask_reg = r[DEPTH_COLOR_MASK], stencil_reg = r[STENCIL_TEST];
        bool depth_test = mask_reg & 1, depth_write = (mask_reg >> 12) & 1 && (r[DEPTH_STENCIL_WRITE] & 2);
        bool stencil = (stencil_reg & 1) && (r[DEPTH_BUFFER_FORMAT] & 3) == 3;
        bool use_depth = depth_address && (depth_test || depth_write || stencil);

        // what the draw reads first, outside a pass
        auto t0 = std::chrono::steady_clock::now();
        VkImageView views[3];
        for (int u = 0; u < 3; u++) views[u] = texture(p, u);
        auto t1 = std::chrono::steady_clock::now();
        Surface *color = surface(color_address, w, h, (r[COLOR_BUFFER_FORMAT] >> 16) & 7, false);
        Surface *depth = use_depth ? surface(depth_address, w, h, r[DEPTH_BUFFER_FORMAT] & 3, true) : nullptr;
        auto t2 = std::chrono::steady_clock::now();
        part[0] += std::chrono::duration<double>(t1 - t0).count();
        part[1] += std::chrono::duration<double>(t2 - t1).count();
        // a texture that is this pass's target cannot be read inside it
        if (rendering && (color_target != color || depth_target != depth)) end_rendering();

        VkDeviceSize vsize = triangles.size() * sizeof(Vertex);
        if (vsize > VERTEX_RING) return;
        VkDeviceSize voff = take(vertices, vertex_at, vsize, 16);
        std::memcpy(vertices.mapped + voff, triangles.data(), vsize);
        State st{};
        for (size_t i = 0; i < std::size(STATE_REGS); i++) st.regs[i] = r[STATE_REGS[i]];
        st.depth[0] = f24(r[DEPTH_RANGE]);
        st.depth[1] = f24(r[DEPTH_NEAR]);
        st.depth[2] = float(r[DEPTHMAP_ENABLE] & 1);
        VkDeviceSize uoff = take(uniforms, uniform_at, sizeof st, 256);
        std::memcpy(uniforms.mapped + uoff, &st, sizeof st);

        if (!rendering) {
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = color->image.view;
            ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo da = ca;
            if (depth) da.imageView = depth->image.view;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {w, h}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            ri.pDepthAttachment = depth ? &da : nullptr;
            ri.pStencilAttachment = depth ? &da : nullptr;
            vkCmdBeginRendering(cmd, &ri);
            rendering = true;
            color_target = color, depth_target = depth;
        }

        // the pipeline: blending (or not, for the logic op) and the color mask
        bool blend = (r[COLOR_OPERATION] >> 8) & 1;
        uint32_t color_mask = r[COLOR_BUFFER_WRITE] & 0xF ? (mask_reg >> 8) & 15 : 0;
        uint64_t key = uint64_t(blend) | uint64_t(color_mask) << 4 | uint64_t(blend ? r[BLEND_FUNC] & 0xFFFF0707 : 0) << 8 |
                       uint64_t(depth != nullptr) << 40;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key, depth != nullptr));

        // the viewport, rows the PICA's way up
        float vw = f24(r[VIEWPORT_WIDTH]) * 2, vh = f24(r[VIEWPORT_HEIGHT]) * 2;
        float vx = float(int32_t(r[VIEWPORT_XY] << 22) >> 22), vy = float(int32_t((r[VIEWPORT_XY] >> 16) << 22) >> 22);
        VkViewport viewport{vx, float(h) - (vy + vh), vw, vh, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {w, h}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        uint32_t cull = r[FACE_CULLING] & 3;
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetCullMode(cmd, cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
        static constexpr VkCompareOp COMPARE[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
                                                   VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                                   VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
        vkCmdSetDepthTestEnable(cmd, depth && (depth_test || depth_write));
        vkCmdSetDepthWriteEnable(cmd, depth && depth_write);
        vkCmdSetDepthCompareOp(cmd, depth_test ? COMPARE[(mask_reg >> 4) & 7] : VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilTestEnable(cmd, depth && stencil);
        static constexpr VkStencilOp OPS[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                              VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                              VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
        uint32_t ops = r[STENCIL_OP];
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, OPS[ops & 7], OPS[(ops >> 8) & 7], OPS[(ops >> 4) & 7],
                          COMPARE[(stencil_reg >> 4) & 7]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_reg >> 24);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, r[DEPTH_STENCIL_WRITE] & 1 ? (stencil_reg >> 8) & 0xFF : 0);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (stencil_reg >> 16) & 0xFF);
        uint32_t bc = r[BLEND_COLOR];
        float constants[4] = {(bc & 0xFF) / 255.0f, (bc >> 8 & 0xFF) / 255.0f, (bc >> 16 & 0xFF) / 255.0f, (bc >> 24) / 255.0f};
        vkCmdSetBlendConstants(cmd, constants);

        VkDescriptorBufferInfo state_info{uniforms.buffer, uoff, sizeof st};
        VkDescriptorImageInfo images[3];
        for (int u = 0; u < 3; u++) images[u] = {sampler(r[UNIT_BASE[u] + 2]), views[u], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &state_info;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 3;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = images;
        push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, writes);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &voff);
        vkCmdDraw(cmd, uint32_t(triangles.size()), 1, 0, 0);
        self.triangles += triangles.size() / 3;

        // drawn: memory is behind until written back, which the CPU asks for
        // through the guards
        color->dirty = true;
        color->version++;
        guard(*color);
        if (depth && (depth_write || stencil)) {
            depth->dirty = true;
            depth->version++;
            guard(*depth);
        }
    }

    double part[2]{}; // textures, surfaces

    void guard(Surface &s) {
        uint32_t paddr = s.address, size = s.bytes();
        uint32_t vaddr = paddr >= map::FCRAM_PADDR ? memory.linear_base + (paddr - map::FCRAM_PADDR)
                                                   : map::VRAM_VADDR + (paddr - map::VRAM_PADDR);
        memory.guard(vaddr, size, false);
        memory.guard(vaddr, size, true);
    }

    void shutdown() {
        if (!device) return;
        if (std::getenv("RUNTIME_VK_DEBUG")) std::fprintf(stderr, "render parts: textures %.1f s, surfaces %.1f s\n", part[0], part[1]);
        vkDeviceWaitIdle(device);
        for (auto &s : surfaces) destroy(s->image), destroy(s->as_color);
        for (auto &[k, t] : textures) destroy(t.image);
        destroy(blank);
        for (auto &[k, pipe] : pipelines) vkDestroyPipeline(device, pipe, nullptr);
        for (auto &[k, s] : samplers) vkDestroySampler(device, s, nullptr);
        for (Buffer &b : staging) destroy(b);
        destroy(vertices);
        destroy(uniforms);
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        vkDestroyShaderModule(device, depth_module, nullptr);
        vkDestroyPipeline(device, depth_pipeline, nullptr);
        vkDestroyPipelineLayout(device, depth_layout, nullptr);
        vkDestroyDescriptorSetLayout(device, depth_set, nullptr);
        vkDestroySampler(device, nearest, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        device = VK_NULL_HANDLE;
    }
};

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
