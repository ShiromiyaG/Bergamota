// The Vulkan device: set up, buffers and images, recording and submitting. (impl.h)

#include "impl.h"

namespace runtime {

namespace {

const uint32_t VERT_SPV[] = {
#include "pica.vert.inc"
};
const uint32_t VERT_GPU_SPV[] = {
#include "pica_gpu.vert.inc"
};
const uint32_t VERT_GPU_NO_STORE_SPV[] = {
#include "pica_gpu_nostore.vert.inc"
};
const uint32_t FRAG_SPV[] = {
#include "pica.frag.inc"
};
const uint32_t FRAG_WBUFFER_SPV[] = {
#include "pica_wbuffer.frag.inc"
};
const uint32_t FRAG_SHADOW_SPV[] = {
#include "pica_shadow.frag.inc"
};
const uint32_t DEPTH_SPV[] = {
#include "depth.comp.inc"
};

} // namespace

using Impl = VulkanRenderer::Impl;

bool Impl::fail(const char *what, VkResult r) {
    self.error = std::string(what) + " failed (" + std::to_string(int(r)) + ")";
    return false;
}

uint32_t Impl::memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
        if ((bits >> i & 1) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return 0;
}

bool Impl::init() {
    // the system's Vulkan loader, its functions through volk
    if (VkResult r = volkInitialize()) return fail("loading Vulkan", r);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "runtime";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("RUNTIME_VK_VALIDATION")) ici.enabledLayerCount = 1, ici.ppEnabledLayerNames = &validation;
    std::vector<const char *> wanted;
    for (const std::string &e : instance_extensions) wanted.push_back(e.c_str());
    ici.enabledExtensionCount = uint32_t(wanted.size());
    ici.ppEnabledExtensionNames = wanted.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &instance)) return fail("vkCreateInstance", r);
    volkLoadInstance(instance);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> all(count);
    vkEnumeratePhysicalDevices(instance, &count, all.data());
    // what a device has of the extensions asked for
    auto extensions_of = [](VkPhysicalDevice d) {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> list(n);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &n, list.data());
        std::vector<std::string> names;
        for (const VkExtensionProperties &e : list) names.emplace_back(e.extensionName);
        return names;
    };
    auto in = [](const std::vector<std::string> &names, const char *name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    // what 1.3 took in that the renderer needs, as extensions on 1.1 and
    // 1.2 (dynamic rendering, with what it needs, synchronization2, the
    // timeline semaphores 1.2 took in), and the ones it needs on any
    static const char *const BEFORE_1_3[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
                                             VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
    static const char *const BEFORE_1_2[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
    // (push descriptors are used where there are, not needed: push())
    static const char *const ALWAYS[] = {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};
    // why a device cannot run it, empty when it can
    auto lacks = [&](VkPhysicalDevice d) -> std::string {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_1) return std::string(p.deviceName) + " has Vulkan 1.0";
        std::vector<std::string> has = extensions_of(d);
        std::string missing;
        auto need = [&](const char *name) {
            if (!in(has, name)) missing += (missing.empty() ? "" : ", ") + std::string(name);
        };
        for (const char *e : ALWAYS) {
            // extended dynamic state is 1.3's own, named or not
            if (p.apiVersion >= VK_API_VERSION_1_3 && std::string(e) == VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) continue;
            need(e);
        }
        if (p.apiVersion < VK_API_VERSION_1_3)
            for (const char *e : BEFORE_1_3) need(e);
        if (p.apiVersion < VK_API_VERSION_1_2)
            for (const char *e : BEFORE_1_2) need(e);
        return missing.empty() ? std::string() : std::string(p.deviceName) + " lacks " + missing;
    };
    // a discrete GPU first, then any other that can
    std::string why;
    for (int pass = 0; pass < 2 && !physical; pass++) {
        for (VkPhysicalDevice d : all) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            if (std::string no = lacks(d); !no.empty()) {
                if (pass == 1) why += (why.empty() ? "" : "; ") + no;
                continue;
            }
            physical = d;
            break;
        }
    }
    if (!physical) {
        self.error = "no GPU the renderer can use" + (why.empty() ? std::string() : " (" + why + ")");
        return false;
    }
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(physical, &p);
        api_version = std::min<uint32_t>(p.apiVersion, VK_API_VERSION_1_3);
        vendor = p.vendorID;
        std::fprintf(stderr, "vulkan: %s, Vulkan %u.%u, driver %08x\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                     VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion);
    }
    const std::vector<std::string> device_extensions = extensions_of(physical);
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
    // vertex shaders that write (the outputs kept for the frames between)
    VkPhysicalDeviceFeatures has{};
    vkGetPhysicalDeviceFeatures(physical, &has);
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.features.vertexPipelineStoresAndAtomics = has.vertexPipelineStoresAndAtomics;
    // the shadow output's atomics on its map
    features.features.fragmentStoresAndAtomics = has.fragmentStoresAndAtomics;
    // (RUNTIME_NO_VERTEX_STORES=1: as on a device without, to compare)
    store_outputs = has.vertexPipelineStoresAndAtomics && !std::getenv("RUNTIME_NO_VERTEX_STORES");
    features.features.vertexPipelineStoresAndAtomics = store_outputs;
    // HD textures as DDS, kept compressed (BC1, BC3, BC7)
    features.features.textureCompressionBC = has.textureCompressionBC;
    bc_textures = has.textureCompressionBC;
    // each feature by its own structure, which 1.1's devices take with the
    // extension and 1.3's as their own
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    dynamic.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    sync2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    eds.extendedDynamicState = VK_TRUE;
    features.pNext = &timeline;
    timeline.pNext = &dynamic;
    dynamic.pNext = &sync2;
    sync2.pNext = &eds;
    // the extensions: those it needs on any, those 1.3 (1.2) took in on an
    // older device, and the swapchain, for a window
    can_present = !instance_extensions.empty();
    std::vector<const char *> extensions;
    if (std::find(device_extensions.begin(), device_extensions.end(), VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) != device_extensions.end())
        extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    // extended dynamic state: 1.3's own, the extension's (and its feature's
    // structure) only where the device names it
    if (std::find(device_extensions.begin(), device_extensions.end(), VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) != device_extensions.end())
        extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    else
        sync2.pNext = nullptr;
    if (api_version < VK_API_VERSION_1_3) extensions.insert(extensions.end(), std::begin(BEFORE_1_3), std::end(BEFORE_1_3));
    if (api_version < VK_API_VERSION_1_2) extensions.insert(extensions.end(), std::begin(BEFORE_1_2), std::end(BEFORE_1_2));
    if (can_present) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device)) return fail("vkCreateDevice", r);
    volkLoadDevice(device);
    // on an older device, 1.3's (1.2's) names are its extensions' (the same
    // functions, as the extensions defined them)
    if (api_version < VK_API_VERSION_1_3) {
        vkCmdBeginRendering = vkCmdBeginRenderingKHR;
        vkCmdEndRendering = vkCmdEndRenderingKHR;
        vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
        vkQueueSubmit2 = vkQueueSubmit2KHR;
        vkCmdSetCullMode = vkCmdSetCullModeEXT;
        vkCmdSetFrontFace = vkCmdSetFrontFaceEXT;
        vkCmdSetPrimitiveTopology = vkCmdSetPrimitiveTopologyEXT;
        vkCmdSetViewportWithCount = vkCmdSetViewportWithCountEXT;
        vkCmdSetScissorWithCount = vkCmdSetScissorWithCountEXT;
        vkCmdBindVertexBuffers2 = vkCmdBindVertexBuffers2EXT;
        vkCmdSetDepthTestEnable = vkCmdSetDepthTestEnableEXT;
        vkCmdSetDepthWriteEnable = vkCmdSetDepthWriteEnableEXT;
        vkCmdSetDepthCompareOp = vkCmdSetDepthCompareOpEXT;
        vkCmdSetDepthBoundsTestEnable = vkCmdSetDepthBoundsTestEnableEXT;
        vkCmdSetStencilTestEnable = vkCmdSetStencilTestEnableEXT;
        vkCmdSetStencilOp = vkCmdSetStencilOpEXT;
    }
    if (api_version < VK_API_VERSION_1_2) {
        vkWaitSemaphores = vkWaitSemaphoresKHR;
        vkSignalSemaphore = vkSignalSemaphoreKHR;
        vkGetSemaphoreCounterValue = vkGetSemaphoreCounterValueKHR;
    }
    vkGetDeviceQueue(device, family, 0, &queue);
    push_descriptors = PFN_vkCmdPushDescriptorSetKHR(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
    // RUNTIME_NO_PUSH_DESCRIPTORS=1: as on a device without them, for testing
    if (const char *off = std::getenv("RUNTIME_NO_PUSH_DESCRIPTORS"); off && off[0] == "1"[0]) push_descriptors = nullptr;

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
        // vertices shaded on the CPU, or the inputs and indices of those
        // shaded on the GPU
        s.vertices = buffer(VERTEX_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        // the state and lighting blocks, and the lighting tables read as storage
        s.uniforms = buffer(UNIFORM_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    }
    for (Shown &sh : shown) {
        for (Buffer &b : sh.buffers) b = buffer(400 * 240 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    cmd = slots[0].cmd;
    vertices = slots[0].vertices;
    uniforms = slots[0].uniforms;
    {
        // timestamps, where the queue keeps them
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fams(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fams.data());
        if (props.limits.timestampPeriod > 0 && fams[family].timestampValidBits >= 32) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = QUERIES * 2;
            vkCreateQueryPool(device, &qi, nullptr, &timestamps);
            timestamp_ns = props.limits.timestampPeriod;
        }
    }

    // the one layout: the state, the three textures, the lighting and
    // its tables; for the vertex shader on the GPU, the program, the
    // uniforms and the inputs, this frame's and the frame before's
    constexpr VkShaderStageFlags V = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutBinding bindings[12] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, V | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, V, nullptr},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, V, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, V, nullptr},
        {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, V, nullptr},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, V, nullptr},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, V, nullptr},
        // a cube map's faces past texture 0
        {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        // the shadow map the shadow output writes, a word a pixel
        {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.flags = pushed_layout();
    sli.bindingCount = 12;
    sli.pBindings = bindings;
    vkCreateDescriptorSetLayout(device, &sli, nullptr, &set_layout);
    VkPushConstantRange push{V, 0, sizeof(VertexPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    vkCreatePipelineLayout(device, &pli, nullptr, &layout);
    pli.pushConstantRangeCount = 0;
    pli.pPushConstantRanges = nullptr;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof VERT_SPV;
    smi.pCode = VERT_SPV;
    vkCreateShaderModule(device, &smi, nullptr, &vert);
    if (store_outputs) {
        // the outputs: on the GPU only, written and read by shaders
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = OUTPUT_HALF * 2;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(device, &bi, nullptr, &outputs);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, outputs, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &ai, nullptr, &outputs_memory) == VK_SUCCESS) {
            vkBindBufferMemory(device, outputs, outputs_memory, 0);
        } else {
            vkDestroyBuffer(device, outputs, nullptr);
            outputs = VK_NULL_HANDLE;
            store_outputs = false;
        }
    }
    // a vertex shader that writes needs the device's leave: without it the
    // shaders made with no outputs to keep (NO_STORE)
    if (!store_outputs) cpu_until_made = true;
    smi.codeSize = store_outputs ? sizeof VERT_GPU_SPV : sizeof VERT_GPU_NO_STORE_SPV;
    smi.pCode = store_outputs ? VERT_GPU_SPV : VERT_GPU_NO_STORE_SPV;
    vkCreateShaderModule(device, &smi, nullptr, &vert_gpu);
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &type;
        vkCreateSemaphore(device, &si, nullptr, &rendered);
        vkCreateSemaphore(device, &si, nullptr, &presented);
    }
    VkPipelineCacheCreateInfo pci_cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(device, &pci_cache, nullptr, &pipeline_cache);
    smi.codeSize = sizeof FRAG_SPV;
    smi.pCode = FRAG_SPV;
    vkCreateShaderModule(device, &smi, nullptr, &frag);
    smi.codeSize = sizeof FRAG_WBUFFER_SPV;
    smi.pCode = FRAG_WBUFFER_SPV;
    vkCreateShaderModule(device, &smi, nullptr, &frag_wbuffer);
    smi.codeSize = sizeof FRAG_SHADOW_SPV;
    smi.pCode = FRAG_SHADOW_SPV;
    vkCreateShaderModule(device, &smi, nullptr, &frag_shadow);

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

    if (!std::getenv("RUNTIME_NO_TRANSLATE")) {
        translator = std::make_unique<Translator>(device, [this](uint64_t key, bool has_depth, VkShaderModule vertex, const uint32_t *shape) {
            return make_pipeline(key, has_depth, vertex, shape);
        }, store_outputs);
    }
    begin();
    blank = image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    uint8_t zero[4] = {};
    upload_color(blank, zero, 4);
    return true;
}

// read: the CPU reads it back, so it is cached on the CPU's side (reads
// of write-combined memory crawl)
Buffer Impl::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool read) {
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
    // a buffer that cannot be had is the end: said, rather than written
    // through a pointer that is not one
    VkResult r = vkAllocateMemory(device, &ai, nullptr, &b.memory);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(device, b.buffer, b.memory, 0);
    if (r == VK_SUCCESS) r = vkMapMemory(device, b.memory, 0, size, 0, reinterpret_cast<void **>(&b.mapped));
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "vulkan: no host memory for a buffer of %llu bytes (%d)\n", (unsigned long long)size, int(r));
        std::abort();
    }
    b.read = read;
    return b;
}

Buffer Impl::staging_buffer(VkDeviceSize size, bool read) {
    int best = -1;
    for (int i = 0; i < int(spare.size()); i++) {
        if (spare[i].read == read && spare[i].size >= size && (best < 0 || spare[i].size < spare[best].size)) best = i;
    }
    if (best >= 0) {
        Buffer b = spare[best];
        spare.erase(spare.begin() + best);
        return b;
    }
    VkDeviceSize rounded = std::bit_ceil(std::max<VkDeviceSize>(size, 64 << 10));
    return buffer(rounded, read ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT, read);
}

void Impl::give_back(Buffer &b) {
    if (!b.buffer) return;
    if (spare.size() < 32) spare.push_back(b), b = {};
    else destroy(b);
}

void Impl::destroy(Buffer &b) {
    if (!b.buffer) return;
    vkDestroyBuffer(device, b.buffer, nullptr);
    vkFreeMemory(device, b.memory, nullptr);
    b = {};
}

// images stay in the general layout: attachment, sampled and copied alike
Image Impl::image(uint32_t w, uint32_t h, VkFormat format, bool depth) {
    Image i;
    i.width = w, i.height = h;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    // RGBA8 seen as a word a pixel too (the shadow output's atomics)
    if (format == VK_FORMAT_R8G8B8A8_UNORM) ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
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

VkImageView Impl::words_view(Image &i) {
    if (!i.words) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32_UINT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vi, nullptr, &i.words);
    }
    return i.words;
}

void Impl::destroy(Image &i) {
    if (!i.image) return;
    vkDestroyImageView(device, i.view, nullptr);
    if (i.words) vkDestroyImageView(device, i.words, nullptr);
    if (i.depth) vkDestroyImageView(device, i.depth, nullptr), vkDestroyImageView(device, i.stencil, nullptr);
    vkDestroyImage(device, i.image, nullptr);
    vkFreeMemory(device, i.memory, nullptr);
    i = {};
}

void Impl::push(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, VkDescriptorSetLayout set, uint32_t count,
                const VkWriteDescriptorSet *writes, std::vector<VkDescriptorPool> &pools, size_t &at) {
    if (push_descriptors) {
        push_descriptors(cmd, point, layout, 0, count, writes);
        return;
    }
    // room for a few hundred draws in each pool, of the layout that takes
    // the most of each kind (the draws': device.cpp's bindings)
    constexpr uint32_t SETS = 512;
    VkDescriptorSet made = VK_NULL_HANDLE;
    for (int tries = 0; tries < 2 && !made; tries++) {
        if (at == pools.size()) {
            VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * SETS},
                                             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * SETS},
                                             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * SETS},
                                             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS}};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = SETS;
            dpi.poolSizeCount = 4;
            dpi.pPoolSizes = sizes;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &dpi, nullptr, &pool) != VK_SUCCESS) return;
            pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pools[at];
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &set;
        if (vkAllocateDescriptorSets(device, &dai, &made) != VK_SUCCESS) made = VK_NULL_HANDLE, at++; // full: the next one
    }
    if (!made) return;
    VkWriteDescriptorSet into[16];
    count = std::min<uint32_t>(count, 16);
    for (uint32_t i = 0; i < count; i++) into[i] = writes[i], into[i].dstSet = made;
    vkUpdateDescriptorSets(device, count, into, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, point, layout, 0, 1, &made, 0, nullptr);
}

void Impl::empty_sets(std::vector<VkDescriptorPool> &pools, size_t &at) {
    for (size_t i = 0; i < pools.size() && i <= at; i++) vkResetDescriptorPool(device, pools[i], 0);
    at = 0;
}

void Impl::begin() {
    if (recording) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    recording = true;
    if (timestamps) {
        uint32_t base = uint32_t(current) * QUERIES;
        vkCmdResetQueryPool(cmd, timestamps, base, QUERIES);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, base);
        queries_used[current] = 2;
        scopes[current].clear();
    }
}

int Impl::gpu_open(int part) {
    if (!timestamps || queries_used[current] + 2 > QUERIES) return -1;
    uint32_t at = uint32_t(current) * QUERIES + queries_used[current];
    queries_used[current] += 2;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps, at);
    scopes[current].push_back({part, at});
    return int(at);
}

void Impl::gpu_close(int at) {
    if (at >= 0) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps, uint32_t(at) + 1);
}

// everything waits for everything before: between passes, copies and
// reads of what was drawn
void Impl::barrier() {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &di);
}

void Impl::end_rendering() {
    if (!rendering) return;
    self.stats.pass_ends[end_cause]++;
    end_cause = 2;
    vkCmdEndRendering(cmd);
    gpu_close(pass_scope);
    pass_scope = -1;
    rendering = false;
    shadow_pass = false;
    color_target = depth_target = nullptr;
    barrier();
}

// runs what is recorded, and waits for it
// what a slot's commands held, once they ran
void Impl::retire(Slot &s) {
    if (!s.pending) return;
    vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &s.fence);
    if (timestamps) {
        size_t slot = size_t(&s - slots);
        uint32_t base = uint32_t(slot) * QUERIES, used = queries_used[slot];
        std::vector<uint64_t> ticks(used);
        if (used && vkGetQueryPoolResults(device, timestamps, base, used, used * 8, ticks.data(), 8, VK_QUERY_RESULT_64_BIT) ==
                        VK_SUCCESS) {
            if (ticks[1] > ticks[0]) self.stats.gpu_seconds += double(ticks[1] - ticks[0]) * timestamp_ns * 1e-9;
            for (auto [part, at] : scopes[slot]) {
                uint64_t a = ticks[at - base], b = ticks[at - base + 1];
                if (b > a) self.stats.gpu_parts[part] += double(b - a) * timestamp_ns * 1e-9;
            }
        }
    }
    for (Buffer &b : s.staging) give_back(b);
    s.staging.clear();
    for (Image &i : s.dead) destroy(i);
    s.dead.clear();
    s.pending = false;
}

// sends what is recorded to the GPU; wait, to have it done (a CPU reads
// its results), else on to the other slot while it runs
void Impl::submit(bool wait) {
    begin();
    end_rendering();
    if (timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps, uint32_t(current) * QUERIES + 1);
    vkEndCommandBuffer(cmd);
    Slot &s = slots[current];
    // the submission's serial on the rendered timeline, for a presenter
    uint64_t serial = submitted + 1;
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &serial;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered;
    {
        std::lock_guard g(queue_lock);
        vkQueueSubmit(queue, 1, &si, s.fence);
    }
    s.pending = true;
    s.serial = ++submitted;
    s.staging.insert(s.staging.end(), staging.begin(), staging.end());
    s.dead.insert(s.dead.end(), dead.begin(), dead.end());
    staging.clear();
    dead.clear();
    recording = false;
    self.stats.submits++;
    if (wait) retire(s);
    // the other slot, once its commands ran
    current ^= 1;
    Slot &next = slots[current];
    retire(next);
    vkResetCommandBuffer(next.cmd, 0);
    empty_sets(next.pools, next.pool_at);
    cmd = next.cmd;
    vertices = next.vertices;
    uniforms = next.uniforms;
    vertex_at = uniform_at = 0;
    begin();
}

// room in a ring, submitting what is there when it runs out
VkDeviceSize Impl::take(Buffer &ring, VkDeviceSize &at, VkDeviceSize size, VkDeviceSize align) {
    at = (at + align - 1) / align * align;
    if (at + size > ring.size) submit();
    VkDeviceSize where = at;
    at += size;
    return where;
}

void Impl::shutdown() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    translator.reset();
    if (pipeline_cache) {
        // what the driver made of the pipelines, for the next run
        size_t size = 0;
        vkGetPipelineCacheData(device, pipeline_cache, &size, nullptr);
        std::vector<uint8_t> data(size);
        if (size && !cache_folder.empty() && vkGetPipelineCacheData(device, pipeline_cache, &size, data.data()) == VK_SUCCESS) {
            if (FILE *f = open_file(cache_folder / "pipelines.bin", "wb")) std::fwrite(data.data(), 1, size, f), std::fclose(f);
        }
        vkDestroyPipelineCache(device, pipeline_cache, nullptr);
    }
    for (auto &s : surfaces) destroy(s->image), destroy(s->as_color), destroy(s->between);
    for (auto &s : retired) destroy(s->image), destroy(s->as_color), destroy(s->between);
    for (Image &i : dead_held) destroy(i);
    for (Image &i : dead) destroy(i);
    for (Buffer &b : spare) destroy(b);
    for (auto &[k, t] : textures) destroy(t.image);
    destroy(blank);
    for (auto &[k, pipe] : pipelines) vkDestroyPipeline(device, pipe, nullptr);
    for (auto &[k, s] : samplers) vkDestroySampler(device, s, nullptr);
    for (Buffer &b : staging) destroy(b);
    for (Shown &sh : shown) {
        for (Buffer &b : sh.buffers) destroy(b);
    }
    for (Slot &s : slots) {
        s.pending = true;
        vkResetFences(device, 1, &s.fence);
        for (VkDescriptorPool p : s.pools) vkDestroyDescriptorPool(device, p, nullptr);
        for (Buffer &b : s.staging) destroy(b);
        for (Image &i : s.dead) destroy(i);
        destroy(s.vertices);
        destroy(s.uniforms);

        vkDestroyFence(device, s.fence, nullptr);
    }
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, vert_gpu, nullptr);
    vkDestroySemaphore(device, rendered, nullptr);
    vkDestroySemaphore(device, presented, nullptr);
    for (ScreenSet &set : sets) destroy(set.screens[0]), destroy(set.screens[1]);
    if (outputs) vkDestroyBuffer(device, outputs, nullptr), vkFreeMemory(device, outputs_memory, nullptr);
    if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    vkDestroyShaderModule(device, frag_wbuffer, nullptr);
    vkDestroyShaderModule(device, frag_shadow, nullptr);
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

} // namespace runtime
