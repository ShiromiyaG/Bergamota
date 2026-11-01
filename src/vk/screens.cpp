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
    // the resolution asked for, once it settles (a hundredth either way is
    // the same)
    if (float want = std::clamp(self.scale.load(), 1.0f, 8.0f); std::abs(want - res) > 0.01f) rescale(want);
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
    // the pictures drawn between the frame before and the one showing now
    // (frame_end), from the blank it shows on, `between` a blank in turn,
    // then the game's own. planned for a frame two blanks or more after the
    // one before, they go unused when it showed a blank after it all the
    // same
    if (new_frame) {
        sequence = between_ready && top == between_address && (!new_before || between_blanks == 1) ? between_count : 0;
        sequence_list = between_list;
        sequence_at = 0;
    }
    new_before = new_frame;
    const int parts = std::clamp(between, 1, PARTS);

    end_rendering();
    int screens_part = gpu_open(3);
    const bool to_cpu = self.cpu_pictures.load();
    // the screens' framebuffers; a screen drawn by the CPU, or not yet, is
    // read from memory, once for all the parts
    Surface *fbs[2];
    Picture from_memory[2];
    std::vector<uint8_t> sideways[2];
    for (uint32_t screen = 0; screen < 2; screen++) {
        const Pica::Screen &at = p.screens[screen];
        uint32_t stride = at.stride ? at.stride : 240 * bytes_per_pixel(at.format);
        fbs[screen] = framebuffer(address[screen], stride);
        if (fbs[screen]) continue;
        sync(address[screen], stride * (screen ? 320 : 400), false);
        Picture &picture = from_memory[screen];
        picture = read_screen(memory, screen, address[screen], at.stride, at.format);
        if (can_present && picture.width) {
            // for the sets on its side, as a framebuffer is
            uint32_t rows = screen ? 320 : 400;
            sideways[screen].assign(size_t(240) * rows * 4, 0);
            for (uint32_t x = 0; x < picture.width && x < rows; x++) {
                for (uint32_t y = 0; y < picture.height && y < 240; y++) {
                    std::memcpy(&sideways[screen][(size_t(x) * 240 + (239 - y)) * 4], &picture.rgba[(size_t(y) * picture.width + x) * 4], 4);
                }
            }
            sideways[screen] = resampled(sideways[screen], 240, rows, scaled(240), scaled(rows), 4);
        }
    }
    for (int part = 0; part < parts; part++) {
        Shown &sh = shown[part];
        const int slot = sequence_at;
        sequence_at = std::min(sequence_at + 1, 1 << 20);
        const bool use_between = slot < sequence;
        // a set for the presenter, once it let go of it
        int set_index = -1;
        if (can_present) {
            set_index = next_set;
            next_set = (next_set + 1) % SETS;
            ScreenSet &set = sets[set_index];
            if (uint64_t wait = set.wait.load()) {
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1;
                wi.pSemaphores = &presented;
                wi.pValues = &wait;
                vkWaitSemaphores(device, &wi, 1000000000ull);
            }
            for (int s = 0; s < 2; s++) {
                uint32_t pw = scaled(240), ph = scaled(s ? 320 : 400);
                if (set.screens[s].image && (set.screens[s].width != pw || set.screens[s].height != ph)) let_go(set.screens[s]), set.screens[s] = {};
                if (!set.screens[s].image) set.screens[s] = image(pw, ph, VK_FORMAT_R8G8B8A8_UNORM, false);
            }
            end_rendering();
        }
        for (uint32_t screen = 0; screen < 2; screen++) {
            Surface *fb = fbs[screen];
            sh.on_gpu[screen] = fb != nullptr;
            if (!fb) {
                if (set_index >= 0 && !sideways[screen].empty()) {
                    upload_color(sets[set_index].screens[screen], sideways[screen].data(), sideways[screen].size());
                    sh.size[screen][0] = 240, sh.size[screen][1] = screen ? 320 : 400;
                }
                sh.from_memory[screen] = from_memory[screen];
                continue;
            }
            const auto &list = between_pictures[sequence_list];
            const Image *between_picture = screen == 0 && use_between && size_t(slot) < list.size() ? &list[slot] : nullptr;
            const Image &from = between_picture && between_picture->width == fb->image.width && between_picture->height == fb->image.height
                                    ? *between_picture : fb->image;
            if (to_cpu) {
                VkBufferImageCopy c{};
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageExtent = {from.width, from.height, 1};
                vkCmdCopyImageToBuffer(cmd, from.image, VK_IMAGE_LAYOUT_GENERAL, sh.buffers[screen].buffer, 1, &c);
                sh.image_size[screen][0] = from.width, sh.image_size[screen][1] = from.height;
            }
            if (set_index >= 0) {
                VkImageCopy c{};
                c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                const Image &to = sets[set_index].screens[screen];
                c.extent = {std::min(from.width, to.width), std::min(from.height, to.height), 1};
                vkCmdCopyImage(cmd, from.image, VK_IMAGE_LAYOUT_GENERAL, to.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            }
            sh.size[screen][0] = fb->width, sh.size[screen][1] = fb->height;
        }
        sh.number = p.vblanks;
        sh.between = use_between;
        sh.part = part;
        sh.pending = true;
        sh.to_cpu = to_cpu;
        sh.set = set_index;
    }
    barrier();
    gpu_close(screens_part);
    submit(false);
    for (int part = 0; part < parts; part++) shown[part].serial = submitted;
    return true;
}

// the copies of the blank before, done by now as a rule (waited for
// otherwise), as frames: a part each
void Impl::finish_shown(Pica &p) {
    for (Shown &sh : shown) {
        if (sh.pending) finish_shown(p, sh);
    }
}

void Impl::finish_shown(Pica &p, Shown &sh) {
    sh.pending = false;
    Frame frame;
    frame.number = sh.number;
    frame.between = sh.between;
    frame.part = sh.part;
    frame.set = sh.set;
    frame.ready = sh.serial;
    std::memcpy(frame.size, sh.size, sizeof frame.size);
    if (!sh.to_cpu) {
        // for a presenter on the GPU: nothing to wait for here
        for (uint32_t screen = 0; screen < 2; screen++) {
            if (!sh.on_gpu[screen]) frame.screens[screen] = std::move(sh.from_memory[screen]);
        }
        p.push_frame(std::move(frame));
        return;
    }
    for (Slot &slot : slots) {
        if (slot.pending && slot.serial <= sh.serial) retire(slot);
    }
    for (uint32_t screen = 0; screen < 2; screen++) {
        Picture &out = frame.screens[screen];
        if (!sh.on_gpu[screen]) {
            out = std::move(sh.from_memory[screen]);
            continue;
        }
        uint32_t w = sh.size[screen][0], h = sh.size[screen][1];
        uint32_t pw = sh.image_size[screen][0], ph = sh.image_size[screen][1];
        out.width = screen ? 320 : 400;
        out.height = 240;
        out.rgba.assign(out.width * out.height * 4, 0);
        // the framebuffer is the screen turned on its side: a screen column
        // is a row of it, bottom to top
        const uint8_t *src = sh.buffers[screen].mapped;
        for (uint32_t x = 0; x < out.width && x < h; x++) {
            for (uint32_t y = 0; y < out.height && y < w; y++) {
                const uint8_t *px = src + (size_t(middle(x, h, ph)) * pw + middle(w - 1 - y, w, pw)) * 4;
                uint8_t *d = &out.rgba[(size_t(y) * out.width + x) * 4];
                d[0] = px[0], d[1] = px[1], d[2] = px[2], d[3] = 255;
            }
        }
    }
    p.push_frame(std::move(frame));
}

} // namespace runtime
