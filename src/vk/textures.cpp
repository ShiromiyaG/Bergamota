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
            if (!t.image.image) t.image = image(w, h, VK_FORMAT_R8G8B8A8_UNORM, false);
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            decode_texture(format, bytes, w, h, rgba.data());
            upload_color(t.image, rgba.data(), rgba.size());
            t.hash = hash;
        }
    }
    return t.image.view;
}

VkImageView Impl::depth_as_color(Surface &s) {
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

} // namespace runtime
