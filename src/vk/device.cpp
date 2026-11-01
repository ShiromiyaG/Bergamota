// The Vulkan device: set up, buffers and images, recording and submitting. (impl.h)

#include "impl.h"

namespace runtime {

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
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (Slot &s : slots) {
        vkAllocateCommandBuffers(device, &cai, &s.cmd);
        vkCreateFence(device, &fci, nullptr, &s.fence);
        s.vertices = buffer(VERTEX_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        // the state and lighting blocks, and the lighting tables read as storage
        s.uniforms = buffer(UNIFORM_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    }
    for (Buffer &b : shown.buffers) b = buffer(400 * 240 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
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
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &depth_pipeline);
    VkSamplerCreateInfo nsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    vkCreateSampler(device, &nsi, nullptr, &nearest);

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
    vkAllocateMemory(device, &ai, nullptr, &b.memory);
    vkBindBufferMemory(device, b.buffer, b.memory, 0);
    vkMapMemory(device, b.memory, 0, size, 0, reinterpret_cast<void **>(&b.mapped));
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

void Impl::destroy(Image &i) {
    if (!i.image) return;
    vkDestroyImageView(device, i.view, nullptr);
    if (i.depth) vkDestroyImageView(device, i.depth, nullptr), vkDestroyImageView(device, i.stencil, nullptr);
    vkDestroyImage(device, i.image, nullptr);
    vkFreeMemory(device, i.memory, nullptr);
    i = {};
}

void Impl::begin() {
    if (recording) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    recording = true;
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
    vkCmdEndRendering(cmd);
    rendering = false;
    color_target = depth_target = nullptr;
    barrier();
}

// runs what is recorded, and waits for it
// what a slot's commands held, once they ran
void Impl::retire(Slot &s) {
    if (!s.pending) return;
    vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &s.fence);
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
    vkEndCommandBuffer(cmd);
    Slot &s = slots[current];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
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
    for (auto &s : surfaces) destroy(s->image), destroy(s->as_color), destroy(s->between);
    for (Image &i : dead_held) destroy(i);
    for (Image &i : dead) destroy(i);
    for (Buffer &b : spare) destroy(b);
    for (auto &[k, t] : textures) destroy(t.image);
    destroy(blank);
    for (auto &[k, pipe] : pipelines) vkDestroyPipeline(device, pipe, nullptr);
    for (auto &[k, s] : samplers) vkDestroySampler(device, s, nullptr);
    for (Buffer &b : staging) destroy(b);
    for (Buffer &b : shown.buffers) destroy(b);
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

} // namespace runtime
