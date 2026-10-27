// The Vulkan renderer: the PICA's draws, its buffers as images. One device,
// one queue and one command buffer, recorded as the command lists come and
// submitted when something needs what it drew (docs/vulkan-renderer.md).

#include "runtime/vulkan_renderer.h"
#include "runtime/memory.h"
#include "runtime/pica_lighting.h"
#include "runtime/pica_tables.h"

#include "../pica/texture.h"
#include "../pica/vertex.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>

namespace runtime {

using namespace pica;

int vk_logic_op(uint32_t pica_op) {
    static constexpr VkLogicOp OPS[16] = {
        VK_LOGIC_OP_CLEAR,         VK_LOGIC_OP_AND,  VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
        VK_LOGIC_OP_SET,           VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT,
        VK_LOGIC_OP_NAND,          VK_LOGIC_OP_OR,   VK_LOGIC_OP_NOR,         VK_LOGIC_OP_XOR,
        VK_LOGIC_OP_EQUIVALENT,    VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED,
    };
    return OPS[pica_op & 15];
}

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
const uint32_t PRESENT_VERT_SPV[] = {
#include "present.vert.inc"
};
const uint32_t PRESENT_FRAG_SPV[] = {
#include "present.frag.inc"
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
    0x0E1, // 40: the fog's color
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
    bool logic_ops = false; // the device's logicOp feature
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
    // each screen read back into two buffers in turn: a copy is recorded
    // into one, the last one recorded is read once its submission ran
    struct ScreenCopy {
        Buffer buffers[2];
        uint64_t serial[2]{}; // the submission each copy went with, 0 for none
        uint32_t size[2][2]{};
        int next = 0;
    } screen_copies[2];
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

    // the fragment lighting: what was last decoded, and where in the
    // uniform ring it and the tables went, for the submission they went with
    pica::LightingDecoder lighting;
    pica::LightingBlock light_block{};
    VkDeviceSize light_offset = 0, tables_offset = 0;
    uint64_t light_serial = ~0ull, tables_serial = ~0ull;
    pica::ShaderTables shader_tables{}; // lighting's, fog's
    uint64_t tables_sent = ~0ull, tables_decoded = ~0ull; // Pica::tables_version of each

    Impl(VulkanRenderer &s, Memory &m, std::string cache, const PresentTarget *t)
        : self(s), memory(m), target(t), cache_path(std::move(cache)) {}

    // the window the screens are shown in, when there is one: its surface,
    // the swapchain, and the pass that draws the screens into it
    const PresentTarget *target;
    VkSurfaceKHR window_surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    std::vector<VkSemaphore> swap_rendered; // one an image: its drawing done, to show it
    VkSemaphore swap_acquired[2]{};         // one a slot: the image to draw into is free
    bool swap_vsync = true, swap_stale = false;
    VkDescriptorSetLayout present_set = VK_NULL_HANDLE;
    VkPipelineLayout present_layout = VK_NULL_HANDLE;
    VkPipeline present_pipeline = VK_NULL_HANDLE;
    VkShaderModule present_vert = VK_NULL_HANDLE, present_frag = VK_NULL_HANDLE;
    Image from_memory[2]; // a screen read from memory, where no surface has it

    // the pipelines compiled in earlier runs, from cache_path; the driver
    // takes what it can use (its own, for this device and version) and
    // ignores the rest
    std::string cache_path;
    VkPipelineCache cache = VK_NULL_HANDLE;
    uint64_t saved_at = 0; // pipelines made when the cache was last written

    void load_cache() {
        std::vector<char> data;
        if (!cache_path.empty()) {
            std::ifstream in(std::filesystem::path(std::u8string(cache_path.begin(), cache_path.end())), std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(in), {});
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(device, &ci, nullptr, &cache) != VK_SUCCESS && !data.empty()) {
            std::fprintf(stderr, "vulkan: the pipeline cache in %s is not usable, starting a new one\n", cache_path.c_str());
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            vkCreatePipelineCache(device, &ci, nullptr, &cache);
        }
    }

    // writes the cache out, beside and then over the old file, so a run cut
    // short leaves a whole one
    void save_cache() {
        if (cache_path.empty() || !cache) return;
        size_t size = 0;
        vkGetPipelineCacheData(device, cache, &size, nullptr);
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS) return;
        std::filesystem::path path(std::u8string(cache_path.begin(), cache_path.end())), part = path;
        part += ".part";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(data.data(), std::streamsize(size));
            if (!out) return;
        }
        std::filesystem::rename(part, path, ec);
        if (ec) std::fprintf(stderr, "vulkan: could not write the pipeline cache to %s\n", cache_path.c_str());
        saved_at = self.pipelines;
    }

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
        // what the window needs, when there is one to show the screens in
        if (target) {
            ici.enabledExtensionCount = uint32_t(target->instance_extensions.size());
            ici.ppEnabledExtensionNames = target->instance_extensions.data();
        }
        if (VkResult r = vkCreateInstance(&ici, nullptr, &instance)) return fail("vkCreateInstance", r);
        if (target && target->make_surface) window_surface = target->make_surface(instance);
        if (target && !window_surface) std::fprintf(stderr, "vulkan: no surface for the window, the screens read back instead\n");

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
        // one queue for everything: graphics, and showing in the window when
        // there is one (a family that cannot leaves the window out)
        bool found = false;
        for (int pass = 0; pass < 2 && !found; pass++) {
            for (uint32_t i = 0; i < families; i++) {
                if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 shows = VK_FALSE;
                if (window_surface) vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, window_surface, &shows);
                if (pass == 0 && window_surface && !shows) continue;
                family = i;
                found = true;
                if (!shows && window_surface) {
                    std::fprintf(stderr, "vulkan: no queue shows in the window, the screens read back instead\n");
                    vkDestroySurfaceKHR(instance, window_surface, nullptr);
                    window_surface = VK_NULL_HANDLE;
                }
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
        const char *extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                                    VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        // logic ops where the device has them, the output merger's other mode
        VkPhysicalDeviceFeatures has{}, wanted{};
        vkGetPhysicalDeviceFeatures(physical, &has);
        wanted.logicOp = has.logicOp;
        logic_ops = has.logicOp;
        if (!logic_ops) std::fprintf(stderr, "vulkan: no logic ops on this device, drawn as copies\n");
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &v13;
        dci.pEnabledFeatures = &wanted;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = window_surface ? 3 : 2; // the swapchain's with a window
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
        load_cache();

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        vkCreateCommandPool(device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.commandBufferCount = 1;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (Slot &s : slots) {
            vkAllocateCommandBuffers(device, &cai, &s.cmd);
            vkCreateFence(device, &fci, nullptr, &s.fence);
            s.vertices = buffer(VERTEX_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            // the state and lighting blocks, and the lighting tables read as storage
            s.uniforms = buffer(UNIFORM_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        }
        for (ScreenCopy &c : screen_copies) {
            for (Buffer &b : c.buffers) b = buffer(400 * 240 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        cmd = slots[0].cmd;
        vertices = slots[0].vertices;
        uniforms = slots[0].uniforms;

        // the one layout: the state, the three textures, the lighting and
        // its tables
        VkDescriptorSetLayoutBinding bindings[4] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sli.bindingCount = 4;
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
        vkCreateComputePipelines(device, cache, 1, &cpi, nullptr, &depth_pipeline);
        VkSamplerCreateInfo nsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        vkCreateSampler(device, &nsi, nullptr, &nearest);

        begin();
        blank = image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        uint8_t zero[4] = {};
        upload_color(blank, zero, 4);
        if (window_surface && !make_present()) {
            std::fprintf(stderr, "vulkan: the window could not be shown in, the screens read back instead\n");
            drop_present();
        }
        return true;
    }

    // -- showing the screens in the window ------------------------------------

    // the pass that draws a screen into the window, and the swapchain
    bool make_present() {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sli.bindingCount = 1;
        sli.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &sli, nullptr, &present_set);
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &present_set;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        vkCreatePipelineLayout(device, &pli, nullptr, &present_layout);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sizeof PRESENT_VERT_SPV;
        smi.pCode = PRESENT_VERT_SPV;
        vkCreateShaderModule(device, &smi, nullptr, &present_vert);
        smi.codeSize = sizeof PRESENT_FRAG_SPV;
        smi.pCode = PRESENT_FRAG_SPV;
        vkCreateShaderModule(device, &smi, nullptr, &present_frag);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore &s : swap_acquired) vkCreateSemaphore(device, &sci, nullptr, &s);
        // the format first, which the pipeline is made for
        if (!make_swapchain(true)) return false;
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = present_vert, stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = present_frag, stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamic;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &swap_format;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &ri;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &dyn;
        gi.layout = present_layout;
        return vkCreateGraphicsPipelines(device, cache, 1, &gi, nullptr, &present_pipeline) == VK_SUCCESS;
    }

    // the swapchain at the window's size now: waiting for the display with
    // vsync (FIFO), else the latest picture (MAILBOX, else IMMEDIATE); false
    // when the window has no size (minimised)
    bool make_swapchain(bool vsync) {
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, window_surface, &caps) != VK_SUCCESS) return false;
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
        if (swap_format == VK_FORMAT_UNDEFINED) {
            uint32_t n = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, window_surface, &n, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(n);
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, window_surface, &n, formats.data());
            // plain 8-bit channels, as the screens are, not sRGB
            for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
                for (auto &f : formats) {
                    if (f.format == want && swap_format == VK_FORMAT_UNDEFINED) swap_format = want;
                }
            }
            if (swap_format == VK_FORMAT_UNDEFINED && n) swap_format = formats[0].format;
        }
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, window_surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, window_surface, &n, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync) {
            for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
                if (mode == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), want) != modes.end()) mode = want;
            }
        }
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = window_surface;
        ci.minImageCount = std::max(caps.minImageCount, 2u) + 1;
        if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = swap_format;
        ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent = caps.currentExtent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain;
        VkSwapchainKHR made = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(device, &ci, nullptr, &made) != VK_SUCCESS) return false;
        drop_swapchain();
        swapchain = made;
        swap_extent = caps.currentExtent;
        swap_vsync = vsync;
        swap_stale = false;
        vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
        swap_images.resize(n);
        vkGetSwapchainImagesKHR(device, swapchain, &n, swap_images.data());
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkImage img : swap_images) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = img;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = swap_format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            vkCreateImageView(device, &vci, nullptr, &view);
            swap_views.push_back(view);
            VkSemaphore s;
            vkCreateSemaphore(device, &sci, nullptr, &s);
            swap_rendered.push_back(s);
        }
        return true;
    }

    // the swapchain's images let go (the swapchain itself only when no newer
    // one replaced it: drop_present)
    void drop_swapchain() {
        if (!swapchain) return;
        vkDeviceWaitIdle(device);
        for (VkImageView v : swap_views) vkDestroyImageView(device, v, nullptr);
        for (VkSemaphore s : swap_rendered) vkDestroySemaphore(device, s, nullptr);
        swap_views.clear();
        swap_rendered.clear();
        swap_images.clear();
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    void drop_present() {
        if (device) vkDeviceWaitIdle(device);
        drop_swapchain();
        if (present_pipeline) vkDestroyPipeline(device, present_pipeline, nullptr);
        if (present_layout) vkDestroyPipelineLayout(device, present_layout, nullptr);
        if (present_set) vkDestroyDescriptorSetLayout(device, present_set, nullptr);
        if (present_vert) vkDestroyShaderModule(device, present_vert, nullptr);
        if (present_frag) vkDestroyShaderModule(device, present_frag, nullptr);
        for (VkSemaphore &s : swap_acquired) {
            if (s) vkDestroySemaphore(device, s, nullptr);
            s = VK_NULL_HANDLE;
        }
        for (Image &i : from_memory) destroy(i);
        present_pipeline = VK_NULL_HANDLE;
        present_layout = VK_NULL_HANDLE;
        present_set = VK_NULL_HANDLE;
        present_vert = present_frag = VK_NULL_HANDLE;
        if (window_surface) vkDestroySurfaceKHR(instance, window_surface, nullptr);
        window_surface = VK_NULL_HANDLE;
    }

    bool presents() const { return present_pipeline != VK_NULL_HANDLE; }

    // the surface a screen is in, as picture() looks for it: rows in order
    // (a display transfer's), up to date, of the LCD's stride
    Surface *screen_surface(uint32_t address, uint32_t stride) {
        Surface *fb = nullptr;
        for (auto &s : surfaces) {
            if (s->linear && !s->stale && !s->depth && s->address == address && s->width * bytes_per_pixel(s->format) == stride)
                fb = s.get();
        }
        return fb && fb->width <= 240 && fb->height <= 400 ? fb : nullptr;
    }

    bool present(const ScreenView *screens, int count, bool smooth, bool vsync,
                 const std::function<void(VkCommandBuffer)> &overlay) {
        if (!presents()) return false;
        if (!swapchain || swap_stale || vsync != swap_vsync) {
            if (!make_swapchain(vsync)) {
                submit(false); // minimised: the frame still goes
                return false;
            }
        }
        begin();
        end_rendering();
        // what each screen is drawn from: its surface, else memory's picture
        VkImageView views[2]{};
        uint32_t rotate[2]{};
        for (int s = 0; s < count && s < 2; s++) {
            // a stride of none is a row of the screen's width (Pica::screen)
            uint32_t stride = screens[s].stride ? screens[s].stride : 240 * bytes_per_pixel(screens[s].format);
            if (Surface *fb = screen_surface(screens[s].address, stride)) {
                views[s] = fb->image.view;
                rotate[s] = 1;
                continue;
            }
            // what the GPU drew there brought down first, as Pica::screen does
            sync(screens[s].address, stride * (s ? 320 : 400), false);
            begin();
            end_rendering();
            Picture p = read_screen(memory, uint32_t(s), screens[s].address, screens[s].stride, screens[s].format);
            if (p.rgba.empty()) {
                views[s] = blank.view;
                continue;
            }
            Image &img = from_memory[s];
            if (img.width != p.width || img.height != p.height) {
                if (img.image) dead.push_back(img);
                img = image(p.width, p.height, VK_FORMAT_R8G8B8A8_UNORM, false);
            }
            upload_color(img, p.rgba.data(), p.rgba.size());
            views[s] = img.view;
        }
        barrier(); // what the game drew, done before it is read

        Slot &slot = slots[current];
        VkSemaphore acquired = swap_acquired[current];
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            swap_stale = true;
            submit(false);
            return false;
        }
        (void)slot;
        VkImageMemoryBarrier2 to_draw{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_draw.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_draw.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_draw.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_draw.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_draw.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_draw.image = swap_images[index];
        to_draw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &to_draw;
        vkCmdPipelineBarrier2(cmd, &di);

        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = swap_views[index];
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0, 0, 0, 1}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, swap_extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport viewport{0, 0, float(swap_extent.width), float(swap_extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, swap_extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline);
        // smooth: linear filtering, clamped (bits 1-2 of a unit's parameters)
        VkSampler filter = sampler(smooth ? 6 : 0);
        for (int s = 0; s < count && s < 2; s++) {
            if (!views[s]) continue;
            VkDescriptorImageInfo info{filter, views[s], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &info;
            push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &w);
            const ScreenView &v = screens[s];
            float W = float(swap_extent.width), H = float(swap_extent.height);
            struct {
                float rect[4];
                uint32_t rotate;
            } push{{v.x / W * 2 - 1, v.y / H * 2 - 1, (v.x + v.width) / W * 2 - 1, (v.y + v.height) / H * 2 - 1}, rotate[s]};
            vkCmdPushConstants(cmd, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20, &push);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
        if (overlay) overlay(cmd);
        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 to_show = to_draw;
        to_show.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_show.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        to_show.dstAccessMask = 0;
        to_show.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_show.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        di.pImageMemoryBarriers = &to_show;
        vkCmdPipelineBarrier2(cmd, &di);

        VkSemaphore rendered = swap_rendered[index];
        submit(false, acquired, rendered);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_stale = true;
        return true;
    }

    // read: the CPU reads it back, so it is cached on the CPU's side (reads
    // of write-combined memory crawl)
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool read = false) {
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
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                 (read ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0));
        if (read && !(memory_props.memoryTypes[ai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
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
    // what a slot's commands held, once they ran
    void retire(Slot &s) {
        if (!s.pending) return;
        vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &s.fence);
        for (Buffer &b : s.staging) destroy(b);
        s.staging.clear();
        for (Image &i : s.dead) destroy(i);
        s.dead.clear();
        s.pending = false;
    }

    // sends what is recorded to the GPU; wait, to have it done (a CPU reads
    // its results), else on to the other slot while it runs
    // (a frame shown in the window waits for its image, acquired, and says
    // when it is drawn, rendered)
    void submit(bool wait = true, VkSemaphore acquired = VK_NULL_HANDLE, VkSemaphore rendered = VK_NULL_HANDLE) {
        begin();
        end_rendering();
        vkEndCommandBuffer(cmd);
        Slot &s = slots[current];
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (acquired) si.waitSemaphoreCount = 1, si.pWaitSemaphores = &acquired, si.pWaitDstStageMask = &stage;
        if (rendered) si.signalSemaphoreCount = 1, si.pSignalSemaphores = &rendered;
        vkQueueSubmit(queue, 1, &si, s.fence);
        s.pending = true;
        s.serial = ++submitted;
        s.staging.insert(s.staging.end(), staging.begin(), staging.end());
        s.dead.insert(s.dead.end(), dead.begin(), dead.end());
        staging.clear();
        dead.clear();
        recording = false;
        self.submits++;
        if (wait) retire(s);
        // the other slot, once its commands ran
        current ^= 1;
        Slot &next = slots[current];
        retire(next);
        vkResetCommandBuffer(next.cmd, 0);
        cmd = next.cmd;
        vertices = next.vertices;
        uniforms = next.uniforms;
        vertex_at = uniform_at = 0;
        begin();
    }

    // a screen, read back from its framebuffer on the GPU a frame behind:
    // the copy goes with this frame, the picture is the last frame's
    bool picture(uint32_t screen, uint32_t address, uint32_t stride, uint32_t format, Picture &out) {
        if (std::getenv("RUNTIME_VK_NO_PICTURE")) return false;
        Surface *fb = nullptr;
        for (auto &s : surfaces) {
            if (s->linear && !s->stale && !s->depth && s->address == address && s->width * bytes_per_pixel(s->format) == stride) fb = s.get();
        }
        if (!fb || fb->width > 240 || fb->height > 400) {
            static int shown = 0;
            if (std::getenv("RUNTIME_VK_DEBUG") && shown++ < 6) {
                std::fprintf(stderr, "picture %u at %08X stride %u fmt %u:", screen, address, stride, format);
                for (auto &s : surfaces)
                    if (overlaps(s->address, s->bytes(), address, stride * 400))
                        std::fprintf(stderr, " [%08X %ux%u f%u lin%d stale%d dirty%d]", s->address, s->width, s->height, s->format, s->linear, s->stale, s->dirty);
                std::fprintf(stderr, "%s", "\n");
            }
            return false;
        }
        (void)format;
        if (std::getenv("RUNTIME_VK_DEBUG")) {
            static int calls = 0;
            calls++;
            if (screen == 0 && calls > 200 && calls < 212) {
                std::fprintf(stderr, "picture call %d: fb %08X %ux%u f%u dirty%d; others:", calls, fb->address, fb->width, fb->height,
                             fb->format, fb->dirty);
                for (auto &o : surfaces) std::fprintf(stderr, " [%08X %ux%u lin%d stale%d dirty%d]", o->address, o->width, o->height, o->linear, o->stale, o->dirty);
                std::fprintf(stderr, "%s", "\n");
            }
        }
        end_rendering();
        ScreenCopy &sc = screen_copies[screen];
        int now = sc.next, last = now ^ 1;
        sc.next = last;
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {fb->width, fb->height, 1};
        vkCmdCopyImageToBuffer(cmd, fb->image.image, VK_IMAGE_LAYOUT_GENERAL, sc.buffers[now].buffer, 1, &c);
        barrier();
        sc.serial[now] = submitted + 1;
        sc.size[now][0] = fb->width, sc.size[now][1] = fb->height;
        // the copy before: its submission done by now, as a rule (frame_done
        // sent it a frame ago), waited for otherwise
        if (!sc.serial[last]) return false;
        if (sc.serial[last] > submitted) submit(true);
        for (Slot &slot : slots) {
            if (slot.pending && slot.serial <= sc.serial[last]) retire(slot);
        }
        uint32_t w = sc.size[last][0], h = sc.size[last][1];
        out.width = screen ? 320 : 400;
        out.height = 240;
        out.rgba.assign(out.width * out.height * 4, 0);
        // the framebuffer is the screen turned on its side: a screen column is
        // a row of it, bottom to top
        const uint8_t *src = sc.buffers[last].mapped;
        for (uint32_t x = 0; x < out.width && x < h; x++) {
            for (uint32_t y = 0; y < out.height && y < w; y++) {
                const uint8_t *p = src + (size_t(x) * w + (w - 1 - y)) * 4;
                uint8_t *d = &out.rgba[(size_t(y) * out.width + x) * 4];
                d[0] = p[0], d[1] = p[1], d[2] = p[2], d[3] = 255;
            }
        }
        return true;
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
        auto started = std::chrono::steady_clock::now();
        uint32_t w = s.width, h = s.height;
        VkDeviceSize texels = VkDeviceSize(w) * h;
        Buffer a = buffer(texels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true), b{};
        end_rendering();
        VkBufferImageCopy c{};
        c.imageSubresource = {VkImageAspectFlags(s.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, s.image.image, VK_IMAGE_LAYOUT_GENERAL, a.buffer, 1, &c);
        if (s.depth) {
            b = buffer(texels, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
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
        if (std::getenv("RUNTIME_VK_DEBUG")) {
            static int shown = 0;
            if (shown++ < 30) {
                std::fprintf(stderr, "write-back %08X %ux%u f%u depth %d linear %d cause %d: %.1f ms\n", s.address, s.width, s.height,
                             s.format, s.depth, s.linear, cause,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            }
        }
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
        // a logic op (bit 41 on, the PICA's operation in bits 42-45)
        cb.logicOpEnable = (key >> 41) & 1;
        cb.logicOp = VkLogicOp(vk_logic_op(uint32_t(key >> 42) & 15));
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
        auto made = std::chrono::steady_clock::now();
        vkCreateGraphicsPipelines(device, cache, 1, &gi, nullptr, &pipe);
        self.time_pipelines += std::chrono::duration<double>(std::chrono::steady_clock::now() - made).count();
        self.pipelines++;
        // written now and then, so a run that does not end well keeps them
        if (self.pipelines - saved_at >= 32) save_cache();
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
        State st{};
        for (size_t i = 0; i < std::size(STATE_REGS); i++) st.regs[i] = r[STATE_REGS[i]];
        st.depth[0] = f24(r[DEPTH_RANGE]);
        st.depth[1] = f24(r[DEPTH_NEAR]);
        // the w buffer is the depth map turned off (Zakuro's DepthMap::read)
        st.depth[2] = float(!(r[DEPTHMAP_ENABLE] & 1));
        pica::LightingBlock lit;
        lighting.decode(r.data(), lit);
        // into the rings; a ring that runs out submits, which leaves what
        // went before in the other slot, so then it all goes again
        VkDeviceSize voff, uoff;
        for (;;) {
            uint64_t serial = submitted;
            voff = take(vertices, vertex_at, vsize, 16);
            std::memcpy(vertices.mapped + voff, triangles.data(), vsize);
            uoff = take(uniforms, uniform_at, sizeof st, 256);
            std::memcpy(uniforms.mapped + uoff, &st, sizeof st);
            // the lighting block and the tables, kept while they and the
            // slot stay the same: most draws in a row share them
            if (light_serial != submitted || std::memcmp(&lit, &light_block, sizeof lit) != 0) {
                light_offset = take(uniforms, uniform_at, sizeof lit, 256);
                std::memcpy(uniforms.mapped + light_offset, &lit, sizeof lit);
                light_block = lit;
                light_serial = submitted;
            }
            if (tables_serial != submitted || tables_sent != p.tables_version) {
                if (tables_decoded != p.tables_version) {
                    pica::decode_light_tables(p.light_tables, shader_tables.light);
                    pica::decode_fog_table(p.fog_table, shader_tables.fog);
                    tables_decoded = p.tables_version;
                }
                tables_offset = take(uniforms, uniform_at, sizeof shader_tables, 256);
                std::memcpy(uniforms.mapped + tables_offset, &shader_tables, sizeof shader_tables);
                tables_sent = p.tables_version;
                tables_serial = submitted;
            }
            if (submitted == serial) break;
        }

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
        // blending off is the logic op's mode; copy (3) is drawing without one
        uint32_t op = r[LOGIC_OP] & 15;
        bool logic = !blend && op != 3 && logic_ops;
        uint64_t key = uint64_t(blend) | uint64_t(color_mask) << 4 | uint64_t(blend ? r[BLEND_FUNC] & 0xFFFF0707 : 0) << 8 |
                       uint64_t(depth != nullptr) << 40 | uint64_t(logic) << 41 | uint64_t(logic ? op : 0) << 42;
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
        VkDescriptorBufferInfo light_info{uniforms.buffer, light_offset, sizeof light_block};
        VkDescriptorBufferInfo tables_info{uniforms.buffer, tables_offset, sizeof shader_tables};
        VkWriteDescriptorSet writes[4]{};
        for (auto &w : writes) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &state_info;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 3;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = images;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &light_info;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &tables_info;
        push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, writes);
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
        drop_present();
        for (auto &s : surfaces) destroy(s->image), destroy(s->as_color);
        for (auto &[k, t] : textures) destroy(t.image);
        destroy(blank);
        for (auto &[k, pipe] : pipelines) vkDestroyPipeline(device, pipe, nullptr);
        save_cache();
        if (cache) vkDestroyPipelineCache(device, cache, nullptr);
        for (auto &[k, s] : samplers) vkDestroySampler(device, s, nullptr);
        for (Buffer &b : staging) destroy(b);
        for (ScreenCopy &c : screen_copies) {
            for (Buffer &b : c.buffers) destroy(b);
        }
        for (Slot &s : slots) {
            s.pending = true;
            vkResetFences(device, 1, &s.fence);
            for (Buffer &b : s.staging) destroy(b);
            for (Image &i : s.dead) destroy(i);
            destroy(s.vertices);
            destroy(s.uniforms);

            vkDestroyFence(device, s.fence, nullptr);
        }
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        vkDestroyShaderModule(device, depth_module, nullptr);
        vkDestroyPipeline(device, depth_pipeline, nullptr);
        vkDestroyPipelineLayout(device, depth_layout, nullptr);
        vkDestroyDescriptorSetLayout(device, depth_set, nullptr);
        vkDestroySampler(device, nearest, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        device = VK_NULL_HANDLE;
    }
};

VulkanRenderer::VulkanRenderer(Memory &memory, std::string pipeline_cache, const PresentTarget *target)
    : impl_(std::make_unique<Impl>(*this, memory, std::move(pipeline_cache), target)) {
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

bool VulkanRenderer::presents() const { return ready() && impl_->presents(); }

VulkanHandles VulkanRenderer::handles() const {
    return {impl_->instance, impl_->physical, impl_->device, impl_->family, impl_->queue, impl_->swap_format,
            uint32_t(impl_->swap_images.size())};
}

bool VulkanRenderer::present(const ScreenView *screens, int count, bool smooth, bool vsync,
                             const std::function<void(VkCommandBuffer)> &overlay) {
    return ready() && impl_->present(screens, count, smooth, vsync, overlay);
}

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

void VulkanRenderer::frame_done() {
    if (ready()) impl_->submit(false);
}

bool VulkanRenderer::picture(uint32_t screen, uint32_t address, uint32_t stride, uint32_t format, Picture &out) {
    return ready() && impl_->picture(screen, address, stride, format, out);
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
