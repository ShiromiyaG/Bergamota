// The PICA's buffers as images: loaded from memory, written back, filled,
// transferred, and the guards that bring them down when the CPU touches them. (impl.h)

#include "impl.h"

namespace runtime {

using Impl = VulkanRenderer::Impl;

void Impl::copy_in(Image &i, const void *bytes, VkDeviceSize size, VkImageAspectFlags aspect) {
    Buffer b = staging_buffer(size, false);
    std::memcpy(b.mapped, bytes, size);
    end_rendering();
    VkBufferImageCopy c{};
    c.imageSubresource = {aspect, 0, 0, 1};
    c.imageExtent = {i.width, i.height, 1};
    vkCmdCopyBufferToImage(cmd, b.buffer, i.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier();
    staging.push_back(b);
    self.stats.uploads++;
}

void Impl::upload_color(Image &i, const uint8_t *rgba, VkDeviceSize size) { copy_in(i, rgba, size, VK_IMAGE_ASPECT_COLOR_BIT); }

// a buffer's memory into its image: rows as memory has them, 8x8 tiles
void Impl::upload(Surface &s) {
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

void Impl::write_back(Surface &s) {
    if (replaying) return; // a frame between the game's never reaches memory
    self.stats.write_back_causes[cause]++;
    auto started = std::chrono::steady_clock::now();
    uint32_t w = s.width, h = s.height;
    VkDeviceSize texels = VkDeviceSize(w) * h;
    Buffer a = staging_buffer(texels * 4, true), b{};
    end_rendering();
    VkBufferImageCopy c{};
    c.imageSubresource = {VkImageAspectFlags(s.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, s.image.image, VK_IMAGE_LAYOUT_GENERAL, a.buffer, 1, &c);
    if (s.depth) {
        b = staging_buffer(texels, true);
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
    give_back(a);
    give_back(b);
    s.dirty = false;
    self.stats.write_backs++;
    if (debug) {
        static int shown = 0;
        if (shown++ < 30) {
            std::fprintf(stderr, "write-back %08X %ux%u f%u depth %d linear %d cause %d: %.1f ms\n", s.address, s.width, s.height,
                         s.format, s.depth, s.linear, cause,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
    }
}

// memory over a range is about to be read or written by something else
void Impl::sync(uint32_t paddr, uint32_t size, bool write) {
    for (auto &s : surfaces) {
        if (!overlaps(s->address, s->bytes(), paddr, size)) continue;
        if (s->dirty) write_back(*s);
        if (write) { // memory has the newer one from now on
            if (color_target == s.get() || depth_target == s.get()) end_rendering();
            s->stale = true;
            if (s->linear) picture_in[s->address] = ++pictures;
        }
    }
}

Surface *Impl::surface(uint32_t address, uint32_t w, uint32_t h, uint32_t format, bool depth, bool linear) {
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
    // a frame drawn again only uses the buffers the game's made
    if (replaying) return nullptr;
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
        let_go(s.image);
        if (s.as_color.image) let_go(s.as_color);
        if (s.between.image) let_go(s.between);
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
void Impl::fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    if (between && !replaying) ops_now.push_back(Op{Op::Fill, {}, {}, {}, {}, 0, {start, end, value, width}, {}});
    for (auto &s : surfaces) {
        if (!overlaps(s->address, s->bytes(), start, end - start)) continue;
        bool whole = start <= s->address && end >= s->address + s->bytes() && !s->linear;
        if (!whole) {
            if (replaying) continue; // memory is the game's, not the frame between's
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
bool Impl::transfer(const Backend::Transfer &t) {
    if (between && !replaying) ops_now.push_back(Op{Op::Transfer, {}, {}, {}, {}, 0, {}, t});
    if (!replaying) picture_in[t.output] = ++pictures;
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
        if (debug && shown++ < 12) {
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
    if (!out || out == in) return false;
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
    // a frame for the top screen (240x400) is done
    if (between && !replaying && t.out_height == 400) frame_end(t.output);
    return true;
}

void Impl::guard(Surface &s) {
    if (replaying) return;
    uint32_t paddr = s.address, size = s.bytes();
    uint32_t vaddr = paddr >= map::FCRAM_PADDR ? memory.linear_base + (paddr - map::FCRAM_PADDR)
                                               : map::VRAM_VADDR + (paddr - map::VRAM_PADDR);
    memory.guard(vaddr, size, false);
    memory.guard(vaddr, size, true);
}

} // namespace runtime
