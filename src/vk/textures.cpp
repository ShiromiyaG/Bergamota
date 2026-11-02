// What a texture unit samples: a buffer drawn there, a depth buffer read as
// color, or a texture decoded from memory. (impl.h)

#include "impl.h"

namespace runtime {

using Impl = VulkanRenderer::Impl;

VkSampler Impl::sampler(uint32_t params) {
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
VkImageView Impl::texture(Pica &p, int unit) {
    if (!(p.regs[TEXTURE_CONFIG] >> unit & 1)) return blank.view;
    return texture_at(p, unit, (p.regs[UNIT_BASE[unit] + 4] & 0x0FFFFFFF) << 3);
}

// a cube map's face 1-5 (-X, +Y, -Y, +Z, -Z; +X is texture 0's own): its
// address the low 22 bits of its register over texture 0's
VkImageView Impl::cube_face(Pica &p, int face) {
    uint32_t main = p.regs[UNIT_BASE[0] + 4] & 0x0FFFFFFF, own = p.regs[UNIT_BASE[0] + 4 + face] & 0x3FFFFF;
    return texture_at(p, 0, ((main & ~0x3FFFFFu) | own) << 3);
}

VkImageView Impl::texture_at(Pica &p, int unit, uint32_t address) {
    uint32_t base = UNIT_BASE[unit], dims = p.regs[base + 1];
    uint32_t h = dims & 0x7FF, w = (dims >> 16) & 0x7FF, format = unit_format(p, unit);
    if (!w || !h || !address) return blank.view;
    // a color buffer drawn into, read in place when it is the same
    for (auto &s : surfaces) {
        if (!s->depth && !s->linear && !s->stale && s->address == address && s->width == w && s->height == h && format <= 4 &&
            s->format == format) {
            if (color_target == s.get()) end_cause = 1, end_rendering();
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
    if (debug) {
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
            // ours in its place, at its own size, or the game's
            const Replacement *r = replacement_files.empty() ? nullptr : replacement(hash);
            const uint32_t iw = r ? r->width : w, ih = r ? r->height : h;
            if (t.image.image && (t.image.width != iw || t.image.height != ih)) let_go(t.image), t.image = {};
            if (!t.image.image) t.image = image(iw, ih, VK_FORMAT_R8G8B8A8_UNORM, false);
            if (r) {
                upload_color(t.image, r->rgba.data(), r->rgba.size());
            } else {
                std::vector<uint8_t> rgba(size_t(w) * h * 4);
                decode_texture(format, bytes, w, h, rgba.data());
                upload_color(t.image, rgba.data(), rgba.size());
                if (save_image && dumped.insert(hash).second) {
                    // upright, as texdump writes them: the PICA's rows bottom first
                    std::vector<uint8_t> upright(rgba.size());
                    for (uint32_t y = 0; y < h; y++)
                        std::memcpy(&upright[size_t(y) * w * 4], &rgba[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
                    char name[96];
                    std::snprintf(name, sizeof name, "tex_%ux%u_%016llX_%u.png", w, h, (unsigned long long)hash, format);
                    save_image(dump_folder / name, upright.data(), w, h);
                }
            }
            t.hash = hash;
        }
    }
    return t.image.view;
}

// an HD texture for a hash, read the first time it is asked for
const Impl::Replacement *Impl::replacement(uint64_t hash) {
    auto file = replacement_files.find(hash);
    if (file == replacement_files.end()) return nullptr;
    auto [it, fresh] = replacements.try_emplace(hash);
    Replacement &r = it->second;
    if (fresh) {
        std::vector<uint8_t> upright;
        uint32_t w = 0, h = 0;
        if (load_image && load_image(file->second, upright, w, h) && w && h && w <= 8192 && h <= 8192 &&
            upright.size() >= size_t(w) * h * 4) {
            r.rgba.resize(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; y++)
                std::memcpy(&r.rgba[size_t(y) * w * 4], &upright[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
            r.width = w, r.height = h;
        } else {
            std::fprintf(stderr, "runtime: %s would not read as an image\n", file->second.string().c_str());
        }
    }
    return r.rgba.empty() ? nullptr : &r;
}

VkImageView Impl::depth_as_color(Surface &s) {
    if (!s.as_color.image) s.as_color = image(s.image.width, s.image.height, VK_FORMAT_R8G8B8A8_UNORM, false);
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
    vkCmdDispatch(cmd, (s.image.width + 7) / 8, (s.image.height + 7) / 8, 1);
    barrier();
    s.converted = s.version;
    return s.as_color.view;
}

} // namespace runtime
