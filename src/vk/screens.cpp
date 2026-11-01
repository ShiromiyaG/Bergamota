// What the screens show, a frame each vertical blank. (impl.h)

#include "impl.h"

namespace runtime {

using Impl = VulkanRenderer::Impl;

// the framebuffer a screen shows, when it is on the GPU
Surface *Impl::framebuffer(uint32_t address, uint32_t stride) {
    Surface *fb = nullptr;
    for (auto &s : surfaces) {
        if (s->linear && !s->stale && !s->depth && s->address == address && s->width * bytes_per_pixel(s->format) == stride) fb = s.get();
    }
    return fb && fb->width <= 240 && fb->height <= 400 ? fb : nullptr;
}

bool Impl::vblank(Pica &p) {
    blanks++;
    if (int want = self.between.load(); want != between) {
        // switched: nothing kept from before
        between = want;
        drop_ops();
        draws_before.clear();
        between_ready = false;
    }
    // no frame for the top screen in a while (only the bottom one drawn):
    // nothing to draw between
    if (!ops_now.empty() && blanks - top_blank > 8) drop_ops();
    finish_shown(p);

    // the frame between on the blank a new frame shows (the one from the
    // frame before to it), the game's own on the next; two new ones in a
    // row (a scene at 60) show the game's. a new frame shows when the
    // screen moves to another buffer (a blank or more after its transfer),
    // or when the buffer it shows got one
    // a game that falls behind (the intro's movie, on a slow computer)
    // skips drawing a frame but still flips, to a buffer whose picture is
    // older than the one shown: the newer one stays, as the console,
    // never behind, would show it
    uint32_t address[2];
    for (uint32_t screen = 0; screen < 2; screen++) {
        uint32_t want = p.screens[screen].address, was = showing[screen];
        auto now = picture_in.find(want), before = picture_in.find(was);
        bool older = was && want != was && before != picture_in.end() &&
                     now != picture_in.end() && now->second < before->second;
        address[screen] = showing[screen] = older ? was : want;
    }
    uint32_t top = address[0];
    bool into_shown = top_frames != top_frames_shown && top_output == top;
    bool new_frame = top != top_shown || into_shown;
    top_frames_shown = top_frames;
    top_shown = top;
    bool use_between = between_ready && new_frame && !new_before && top == between_address;
    new_before = new_frame;

    end_rendering();
    for (uint32_t screen = 0; screen < 2; screen++) {
        const Pica::Screen &at = p.screens[screen];
        uint32_t stride = at.stride ? at.stride : 240 * bytes_per_pixel(at.format);
        Surface *fb = framebuffer(address[screen], stride);
        shown.on_gpu[screen] = fb != nullptr;
        if (!fb) {
            // drawn by the CPU, or not yet: memory has it
            sync(address[screen], stride * (screen ? 320 : 400), false);
            shown.from_memory[screen] = read_screen(memory, screen, address[screen], at.stride, at.format);
            continue;
        }
        const Image &from = screen == 0 && use_between && fb->between.image ? fb->between : fb->image;
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {fb->width, fb->height, 1};
        vkCmdCopyImageToBuffer(cmd, from.image, VK_IMAGE_LAYOUT_GENERAL, shown.buffers[screen].buffer, 1, &c);
        shown.size[screen][0] = fb->width, shown.size[screen][1] = fb->height;
    }
    barrier();
    shown.number = p.vblanks;
    shown.between = use_between;
    shown.pending = true;
    submit(false);
    shown.serial = submitted;
    return true;
}

// the copies of the blank before, done by now as a rule (waited for
// otherwise), as a frame
void Impl::finish_shown(Pica &p) {
    if (!shown.pending) return;
    shown.pending = false;
    for (Slot &slot : slots) {
        if (slot.pending && slot.serial <= shown.serial) retire(slot);
    }
    Frame frame;
    frame.number = shown.number;
    frame.between = shown.between;
    for (uint32_t screen = 0; screen < 2; screen++) {
        Picture &out = frame.screens[screen];
        if (!shown.on_gpu[screen]) {
            out = std::move(shown.from_memory[screen]);
            continue;
        }
        uint32_t w = shown.size[screen][0], h = shown.size[screen][1];
        out.width = screen ? 320 : 400;
        out.height = 240;
        out.rgba.assign(out.width * out.height * 4, 0);
        // the framebuffer is the screen turned on its side: a screen column
        // is a row of it, bottom to top
        const uint8_t *src = shown.buffers[screen].mapped;
        for (uint32_t x = 0; x < out.width && x < h; x++) {
            for (uint32_t y = 0; y < out.height && y < w; y++) {
                const uint8_t *px = src + (size_t(x) * w + (w - 1 - y)) * 4;
                uint8_t *d = &out.rgba[(size_t(y) * out.width + x) * 4];
                d[0] = px[0], d[1] = px[1], d[2] = px[2], d[3] = 255;
            }
        }
    }
    p.push_frame(std::move(frame));
}

} // namespace runtime
