// The frames shown in a window through the renderer's own Vulkan device:
// a swapchain on the window's surface, both screens drawn from the
// renderer's copies on the GPU (Frame::set), letterboxed, and an overlay on
// top (Dear ImGui's Vulkan backend, recorded by the caller). No picture goes
// through the CPU (src/vk/present.cpp).
#ifndef RUNTIME_VULKAN_PRESENTER_H
#define RUNTIME_VULKAN_PRESENTER_H

#include "runtime/gpu.h"
#include "runtime/vulkan_renderer.h"

#include <vulkan/vulkan.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace runtime {

class VulkanPresenter {
public:
    // make_surface: the window's surface on the renderer's instance
    // (SDL_Vulkan_CreateSurface); the renderer made with the window's
    // instance extensions
    VulkanPresenter(VulkanRenderer &renderer, const std::function<bool(VkInstance, VkSurfaceKHR *)> &make_surface);
    ~VulkanPresenter();
    bool ready() const { return surface_ != VK_NULL_HANDLE && error.empty(); }
    std::string error;

    struct View {
        uint32_t width = 0, height = 0; // the window's, in pixels
        bool both = true;               // the bottom screen under the top one
        bool smooth = false;            // filtered, else nearest
        bool vsync = true;
        float top = 400; // the top screen's width shown, at its height of 240 (wider for widescreen)
        bool antialias = false; // FXAA over the screens
        float sharpen = 0;      // 0-1
    };
    // the screens' area in a window of width x height (anything), the top
    // screen top wide: the scale and where it starts, for the mouse; the
    // bottom screen centered under the top one
    static void letterbox(float width, float height, bool both, float top, float &scale, float &x, float &y);

    // shows frame (null: the last one again) in the window, then overlay
    // records onto it (inside its rendering)
    void present(const Frame *frame, const View &view, const std::function<void(VkCommandBuffer)> &overlay);

    // for Dear ImGui's Vulkan backend
    VkInstance instance() const;
    VkPhysicalDevice physical() const;
    VkDevice device() const;
    VkQueue queue() const;
    uint32_t family() const;
    VkFormat format() const { return format_; }
    uint32_t image_count() const { return uint32_t(images_.size()); }
    VkDescriptorPool overlay_pool() const { return pool_; }
    std::mutex &queue_lock();
    // waits until the GPU has done all that was sent, the queue to itself
    void idle();

private:
    bool make_swapchain(const View &view);
    void drop_swapchain();

    VulkanRenderer &renderer_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    bool vsync_ = true;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> finished_; // a swapchain image's: its drawing done
    struct InFlight {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    } flight_[2];
    int at_ = 0;
    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    uint64_t presented_ = 0; // the presenter's timeline
    Frame last_;             // what is on the window, to show again
};

} // namespace runtime

#endif
