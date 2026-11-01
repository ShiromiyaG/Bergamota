// The frames shown in a window through the renderer's device
// (include/runtime/vulkan_presenter.h).

#include "runtime/vulkan_presenter.h"
#include "impl.h"

#include <algorithm>

namespace runtime {

namespace {

const uint32_t PRESENT_VERT[] = {
#include "present.vert.inc"
};
const uint32_t PRESENT_FRAG[] = {
#include "present.frag.inc"
};

struct ScreenPush {
    float rect[4];
    float from[4];   // the part of the screen, 0 to 1
    float extent[2]; // the part of the image the screen fills
    float blur, unused;
    float post[4]; // anti-aliasing on, sharpening 0-1, brightness, opacity (present.frag)
};

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

VulkanPresenter::VulkanPresenter(VulkanRenderer &renderer, const std::function<bool(VkInstance, VkSurfaceKHR *)> &make_surface)
    : renderer_(renderer) {
    VulkanRenderer::Impl &d = *renderer.impl_;
    if (!renderer.ready() || !d.can_present) {
        error = "the renderer was made without a window's extensions";
        return;
    }
    if (!make_surface(d.instance, &surface_)) {
        error = "no surface on the window";
        return;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(d.physical, d.family, surface_, &supported);
    if (!supported) {
        error = "the GPU's queue cannot present to this window";
        return;
    }
    // plain 8-bit BGRA where there is one: the screens' values as they are
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, surface_, &count, formats.data());
    if (!formats.empty()) format_ = formats[0].format;
    for (const VkSurfaceFormatKHR &f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) format_ = f.format;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.family;
    vkCreateCommandPool(d.device, &pci, nullptr, &commands_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = commands_;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (InFlight &f : flight_) {
        vkAllocateCommandBuffers(d.device, &cai, &f.cmd);
        vkCreateFence(d.device, &fci, nullptr, &f.fence);
        vkCreateSemaphore(d.device, &sci, nullptr, &f.acquired);
    }

    // a screen: its image and sampler pushed, its rectangle in push constants
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sli.bindingCount = 1;
    sli.pBindings = &binding;
    vkCreateDescriptorSetLayout(d.device, &sli, nullptr, &set_layout_);
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScreenPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    vkCreatePipelineLayout(d.device, &pli, nullptr, &layout_);
    VkShaderModule modules[2];
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof PRESENT_VERT;
    smi.pCode = PRESENT_VERT;
    vkCreateShaderModule(d.device, &smi, nullptr, &modules[0]);
    smi.codeSize = sizeof PRESENT_FRAG;
    smi.pCode = PRESENT_FRAG;
    vkCreateShaderModule(d.device, &smi, nullptr, &modules[1]);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    // a placement see-through by its opacity
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format_;
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
    gi.layout = layout_;
    vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline_);
    vkDestroyShaderModule(d.device, modules[0], nullptr);
    vkDestroyShaderModule(d.device, modules[1], nullptr);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(d.device, &si, nullptr, &nearest_);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(d.device, &si, nullptr, &linear_);

    // the overlay's textures (Dear ImGui's font and whatever it adds)
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 64;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    vkCreateDescriptorPool(d.device, &dpi, nullptr, &pool_);
}

VulkanPresenter::~VulkanPresenter() {
    VulkanRenderer::Impl &d = *renderer_.impl_;
    if (!d.device) return;
    idle();
    drop_swapchain();
    for (InFlight &f : flight_) {
        if (f.fence) vkDestroyFence(d.device, f.fence, nullptr);
        if (f.acquired) vkDestroySemaphore(d.device, f.acquired, nullptr);
    }
    if (commands_) vkDestroyCommandPool(d.device, commands_, nullptr);
    if (pipeline_) vkDestroyPipeline(d.device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d.device, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d.device, set_layout_, nullptr);
    if (nearest_) vkDestroySampler(d.device, nearest_, nullptr);
    if (linear_) vkDestroySampler(d.device, linear_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d.device, pool_, nullptr);
    if (surface_) vkDestroySurfaceKHR(d.instance, surface_, nullptr);
}

VkInstance VulkanPresenter::instance() const { return renderer_.impl_->instance; }
VkPhysicalDevice VulkanPresenter::physical() const { return renderer_.impl_->physical; }
VkDevice VulkanPresenter::device() const { return renderer_.impl_->device; }
VkQueue VulkanPresenter::queue() const { return renderer_.impl_->queue; }
uint32_t VulkanPresenter::family() const { return renderer_.impl_->family; }
std::mutex &VulkanPresenter::queue_lock() { return renderer_.impl_->queue_lock; }

void VulkanPresenter::idle() {
    std::lock_guard g(renderer_.impl_->queue_lock);
    vkQueueWaitIdle(renderer_.impl_->queue);
}

void VulkanPresenter::place_screens(View &view) {
    if (!view.placements.empty()) return;
    view.canvas[0] = view.top, view.canvas[1] = view.both ? 480.0f : 240.0f;
    Placement top;
    top.to[2] = view.top, top.to[3] = 240;
    view.placements.push_back(top);
    if (view.both) {
        Placement bottom;
        bottom.screen = 1;
        bottom.to[0] = (view.top - 320) / 2, bottom.to[1] = 240, bottom.to[2] = bottom.to[0] + 320, bottom.to[3] = 480;
        view.placements.push_back(bottom);
    }
}

void VulkanPresenter::fit(const View &view, float &scale, float &x, float &y) {
    float w = std::max(view.canvas[0], 1.0f), h = std::max(view.canvas[1], 1.0f);
    scale = std::min(float(view.width) / w, float(view.height) / h);
    x = (float(view.width) - w * scale) / 2;
    y = (float(view.height) - h * scale) / 2;
}

bool VulkanPresenter::on_bottom(const View &view, float x, float y, float &bx, float &by) {
    float scale, x0, y0;
    fit(view, scale, x0, y0);
    if (scale <= 0) return false;
    float cx = (x - x0) / scale, cy = (y - y0) / scale;
    for (auto p = view.placements.rbegin(); p != view.placements.rend(); ++p) {
        const float *to = p->to;
        if (cx < to[0] || cx >= to[2] || cy < to[1] || cy >= to[3] || p->opacity <= 0) continue;
        // the topmost under the point takes it, whichever screen it shows
        if (p->screen != 1) return false;
        float u = (cx - to[0]) / (to[2] - to[0]), v = (cy - to[1]) / (to[3] - to[1]);
        bx = (p->from[0] + u * (p->from[2] - p->from[0])) * 320;
        by = (p->from[1] + v * (p->from[3] - p->from[1])) * 240;
        return true;
    }
    return false;
}

void VulkanPresenter::drop_swapchain() {
    VkDevice device = renderer_.impl_->device;
    for (VkImageView v : views_) vkDestroyImageView(device, v, nullptr);
    for (VkSemaphore s : finished_) vkDestroySemaphore(device, s, nullptr);
    views_.clear();
    finished_.clear();
    images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VulkanPresenter::make_swapchain(const View &view) {
    VulkanRenderer::Impl &d = *renderer_.impl_;
    idle();
    drop_swapchain();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, surface_, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = std::clamp(view.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(view.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false; // minimized
    // with vsync the refresh paces it; without, the newest frame each
    // refresh (mailbox) or at once (immediate)
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, surface_, &count, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!view.vsync) mode = has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                            : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                 : VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(d.device, &ci, nullptr, &swapchain_) != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        error = "no swapchain on the window";
        return false;
    }
    extent_ = extent;
    vsync_ = view.vsync;
    vkGetSwapchainImagesKHR(d.device, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(d.device, swapchain_, &count, images_.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view_handle;
        vkCreateImageView(d.device, &vi, nullptr, &view_handle);
        views_.push_back(view_handle);
        VkSemaphore s;
        vkCreateSemaphore(d.device, &sci, nullptr, &s);
        finished_.push_back(s);
    }
    return true;
}

void VulkanPresenter::present(const Frame *frame, const View &view, const std::function<void(VkCommandBuffer)> &overlay) {
    VulkanRenderer::Impl &d = *renderer_.impl_;
    if (!surface_ || !error.empty()) return;
    if (frame) {
        last_.set = frame->set;
        last_.ready = frame->ready;
        std::copy(&frame->size[0][0], &frame->size[0][0] + 4, &last_.size[0][0]);
    }
    if (!view.width || !view.height) return;
    if (!swapchain_ || view.width != extent_.width || view.height != extent_.height || view.vsync != vsync_) {
        if (!make_swapchain(view)) return;
    }
    InFlight &f = flight_[at_];
    vkWaitForFences(d.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(d.device, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain(view);
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(d.device, 1, &f.fence);

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    image_barrier(cmd, images_[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = views_[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport whole{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D all{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &whole);
    vkCmdSetScissor(cmd, 0, 1, &all);

    // the placements on the canvas, fitted to the window
    bool shown = last_.set >= 0;
    if (shown) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        View placed = view;
        placed.width = extent_.width, placed.height = extent_.height;
        place_screens(placed);
        float scale, x0, y0;
        fit(placed, scale, x0, y0);
        for (const Placement &p : placed.placements) {
            const int s = p.screen ? 1 : 0;
            const Image &image = d.sets[last_.set].screens[s];
            if (!image.view || p.opacity <= 0) continue;
            float px0 = x0 + p.to[0] * scale, py0 = y0 + p.to[1] * scale, px1 = x0 + p.to[2] * scale, py1 = y0 + p.to[3] * scale;
            // the blur in the image's texels (the screen's pixels, scaled as it is drawn)
            float texels = p.blur * float(image.width) / 240.0f;
            ScreenPush push{{px0 / extent_.width * 2 - 1, py0 / extent_.height * 2 - 1, px1 / extent_.width * 2 - 1,
                             py1 / extent_.height * 2 - 1},
                            {p.from[0], p.from[1], p.from[2], p.from[3]},
                            {float(last_.size[s][0]) / 240.0f, float(last_.size[s][1]) / (s ? 320.0f : 400.0f)},
                            texels,
                            0,
                            {view.antialias ? 1.0f : 0.0f, view.sharpen, p.brightness, p.opacity}};
            VkDescriptorImageInfo info{view.smooth ? linear_ : nearest_, image.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            d.push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &write);
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }
    if (overlay) overlay(cmd);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cmd);

    // after the renderer's copies of the set, and the set kept from it until done
    uint64_t done = ++presented_;
    if (shown) d.sets[last_.set].wait.store(done);
    VkSemaphore waits[2] = {f.acquired, d.rendered};
    uint64_t wait_values[2] = {0, last_.ready};
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    VkSemaphore signals[2] = {finished_[index], d.presented};
    uint64_t signal_values[2] = {0, done};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = shown ? 2 : 1;
    ti.pWaitSemaphoreValues = wait_values;
    ti.signalSemaphoreValueCount = 2;
    ti.pSignalSemaphoreValues = signal_values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.waitSemaphoreCount = shown ? 2 : 1;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = signals;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &finished_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkResult result;
    {
        std::lock_guard g(d.queue_lock);
        vkQueueSubmit(d.queue, 1, &si, f.fence);
        result = vkQueuePresentKHR(d.queue, &pi);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) extent_ = {}; // made again next time
    at_ ^= 1;
}

} // namespace runtime
