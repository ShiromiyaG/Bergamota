// What a texture unit samples: a buffer drawn there, a depth buffer read as
// color, or a texture decoded from memory. (impl.h)

#include "impl.h"

#include <cctype>
#include <fstream>
#include <iterator>

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
            // a pack's in its place, at its own size, or the game's
            const PackFile *file = packs.empty() ? nullptr : packs.find(format, bytes, w, h, hash);
            const Replacement *r = file ? replacement(*file) : nullptr;
            const uint32_t iw = r ? r->width : w, ih = r ? r->height : h;
            const VkFormat f = r ? r->format : VK_FORMAT_R8G8B8A8_UNORM;
            if (t.image.image && (t.image.width != iw || t.image.height != ih || t.format != f)) {
                upside_down_views.erase(t.image.view);
                let_go(t.image), t.image = {};
            }
            if (!t.image.image) {
                t.image = f == VK_FORMAT_R8G8B8A8_UNORM ? image(iw, ih, f, false) : compressed_image(iw, ih, f);
                t.format = f;
            }
            t.upside_down = r && r->upside_down;
            if (t.upside_down) upside_down_views.insert(t.image.view);
            else upside_down_views.erase(t.image.view);
            if (r) {
                upload_color(t.image, r->rgba.data(), r->rgba.size());
                self.stats.replaced++;
                if (used_files.insert(file).second) self.stats.replacements++;
            } else {
                std::vector<uint8_t> rgba(size_t(w) * h * 4);
                decode_texture(format, bytes, w, h, rgba.data());
                upload_color(t.image, rgba.data(), rgba.size());
                // dumped as Azahar dumps them: named by the new hash, its
                // first row the first in memory, as packs take them
                const uint64_t key = save_image ? raw_texture_hash(bytes, size) : 0;
                if (save_image && dumped.insert(key).second) save_image(dump_folder / dump_name(w, h, key, format), rgba.data(), w, h);
            }
            t.hash = hash;
        }
    }
    return t.image.view;
}

// an HD texture read (empty for a file that would not read). The runtime
// keeps a texture's rows as memory has them, first row first, where Azahar
// keeps them the other way round: so an Azahar pack's PNG, which Azahar
// turns over as it reads it (flip_png_files), is taken here as it is (and
// turned over without flip_png_files), and its DDS or KTX, which Azahar
// takes as it is stored, is taken as it is too and sampled with v turned
// over (Replacement::upside_down). The runtime's own tex_ files from before
// go the other way: a PNG turned over, a DDS as it is
Impl::Replacement Impl::read_replacement(const PackFile &pack) const {
    Replacement r;
    const std::filesystem::path &file = pack.path;
    std::string ext = file.extension().string();
    for (char &c : ext) c = char(std::tolower(uint8_t(c)));
    auto turn_over = [](std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
        std::vector<uint8_t> row(size_t(w) * 4);
        for (uint32_t y = 0; y < h / 2; y++) {
            uint8_t *a = &rgba[size_t(y) * w * 4], *b = &rgba[size_t(h - 1 - y) * w * 4];
            std::memcpy(row.data(), a, row.size()), std::memcpy(a, b, row.size()), std::memcpy(b, row.data(), row.size());
        }
    };
    if (ext == ".dds" || ext == ".ktx") {
        // the first image only: its first level, its rows as they are
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
        auto u32 = [&](size_t at) { return at + 4 <= d.size() ? uint32_t(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24) : 0u; };
        auto cc = [](const char *s) { return uint32_t(uint8_t(s[0]) | uint8_t(s[1]) << 8 | uint8_t(s[2]) << 16 | uint32_t(uint8_t(s[3])) << 24); };
        size_t data = 0;
        uint32_t block = 0, w = 0, h = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool rgba8 = false;
        if (ext == ".dds" && d.size() >= 128 && u32(0) == cc("DDS ")) {
            h = u32(12), w = u32(16);
            data = 128;
            const uint32_t fourcc = u32(84);
            if (fourcc == cc("DX10")) {
                data += 20;
                // sRGB or not, the bits are the same: the game's values, as they are
                switch (u32(128)) {
                case 28: case 29: rgba8 = true; break;
                case 71: case 72: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, block = 8; break;
                case 77: case 78: format = VK_FORMAT_BC3_UNORM_BLOCK, block = 16; break;
                case 83: format = VK_FORMAT_BC5_UNORM_BLOCK, block = 16; break;
                case 98: case 99: format = VK_FORMAT_BC7_UNORM_BLOCK, block = 16; break;
                }
            } else if (fourcc == cc("DXT1")) {
                format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, block = 8;
            } else if (fourcc == cc("DXT5") || fourcc == cc("DXT4")) {
                format = VK_FORMAT_BC3_UNORM_BLOCK, block = 16;
            } else if (fourcc == cc("ATI2") || fourcc == cc("BC5U")) {
                format = VK_FORMAT_BC5_UNORM_BLOCK, block = 16;
            } else if ((u32(80) & 0x40) && u32(88) == 32 && u32(92) == 0xFF && u32(96) == 0xFF00 && u32(100) == 0xFF0000 &&
                       u32(104) == 0xFF000000u) {
                rgba8 = true; // 32 bits, R in the low byte: as RGBA8 is
            }
        } else if (ext == ".ktx" && d.size() >= 64 && std::memcmp(d.data(), "\xABKTX 11\xBB\r\n\x1A\n", 12) == 0 && u32(12) == 0x04030201) {
            // KTX 1: its header, its key and value data, then the first
            // level's size and bytes
            const uint32_t internal = u32(28);
            w = u32(36), h = u32(40);
            data = 64 + size_t(u32(60)) + 4;
            switch (internal) {
            case 0x8058: case 0x1908: rgba8 = true; break;
            case 0x83F0: case 0x83F1: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, block = 8; break;
            case 0x83F3: format = VK_FORMAT_BC3_UNORM_BLOCK, block = 16; break;
            case 0x8C72: case 0x8DBD: format = VK_FORMAT_BC5_UNORM_BLOCK, block = 16; break;
            case 0x8E8C: format = VK_FORMAT_BC7_UNORM_BLOCK, block = 16; break;
            }
        }
        const size_t size = rgba8 ? size_t(w) * h * 4 : size_t((w + 3) / 4) * ((h + 3) / 4) * block;
        if ((!block && !rgba8) || !w || !h || w > 8192 || h > 8192 || data + size > d.size()) {
            std::fprintf(stderr, "runtime: %s is not a DDS or KTX of RGBA8, BC1, BC3, BC5 or BC7\n", file.string().c_str());
        } else if (!rgba8 && !bc_textures) {
            std::fprintf(stderr, "runtime: %s passed over: the GPU has no BC textures\n", file.string().c_str());
        } else {
            r.rgba.assign(d.begin() + long(data), d.begin() + long(data + size));
            r.width = w, r.height = h;
            // as Azahar does, nothing of it moved: an Azahar pack's rows go
            // the other way from the game's here, and it is sampled with v
            // turned over (RGBA8 and blocks alike)
            if (!rgba8) r.format = format;
            r.upside_down = pack.azahar;
        }
        return r;
    }
    uint32_t w = 0, h = 0;
    if (load_image && load_image(file, r.rgba, w, h) && w && h && w <= 8192 && h <= 8192 && r.rgba.size() >= size_t(w) * h * 4) {
        r.rgba.resize(size_t(w) * h * 4);
        if (!pack.azahar || !pack.flip_png) turn_over(r.rgba, w, h);
        r.width = w, r.height = h;
    } else {
        r.rgba.clear();
        std::fprintf(stderr, "runtime: %s would not read as an image\n", file.string().c_str());
    }
    return r;
}

// an HD texture for a pack's file: read already (preloaded, or asked for
// before), else read now. a Replacement stays where it is once in the map
const Impl::Replacement *Impl::replacement(const PackFile &file) {
    {
        std::lock_guard g(replacements_lock);
        if (auto it = replacements.find(&file); it != replacements.end()) return it->second.rgba.empty() ? nullptr : &it->second;
    }
    Replacement read = read_replacement(file);
    std::lock_guard g(replacements_lock);
    Replacement &r = replacements.try_emplace(&file, std::move(read)).first->second;
    return r.rgba.empty() ? nullptr : &r;
}

// every HD texture read on threads of their own, those the game asked for
// first left alone
void Impl::preload_replacements() {
    std::vector<const PackFile *> files = packs.files();
    auto next = std::make_shared<std::atomic<size_t>>(0);
    const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1, 8);
    for (size_t k = 0; k < workers; k++) {
        preloaders.emplace_back([this, files, next] {
            for (size_t i; !stop_preloading && (i = (*next)++) < files.size();) {
                {
                    std::lock_guard g(replacements_lock);
                    if (replacements.count(files[i])) continue;
                }
                Replacement read = read_replacement(*files[i]);
                std::lock_guard g(replacements_lock);
                replacements.try_emplace(files[i], std::move(read));
            }
        });
    }
}

// an image for an HD texture in a compressed format: copied into and
// sampled, nothing else
Image Impl::compressed_image(uint32_t w, uint32_t h, VkFormat format) {
    Image i;
    i.width = w, i.height = h;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &i.view);
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
