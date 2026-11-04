// Texture packs: a game's textures replaced by files found by the hash of
// what they replace. Azahar's (and Citra's) packs as they are, written from
// a description of how they behave (docs/tools.md, "HD textures"), none of
// their code: load/textures/<title id>/, its pack.json, tex1_ names, the
// hash CityHash64 (Google's, v1.1) over a texture level's bytes as memory
// has them (the new hash) or decoded (the legacy one, for a pack without a
// pack.json). And the runtime's own from before, tex_ names with its own
// hash.

#ifndef RUNTIME_TEXTURE_PACK_H
#define RUNTIME_TEXTURE_PACK_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace runtime {

// CityHash64 v1.1, unseeded
uint64_t city_hash64(const void *data, size_t size);
// Azahar's keys for one level of a texture in memory, the PICA's format
// number and size: the new hash, over its bytes as memory has them...
uint64_t raw_texture_hash(const uint8_t *bytes, size_t size);
// ...and the legacy one, over it decoded: rows from the last in memory to
// the first, formats 0-4 their texels' own bytes, 5-13 R, G, B, A
uint64_t decoded_texture_hash(uint32_t format, const uint8_t *bytes, uint32_t width, uint32_t height);

// a file that replaces a texture
struct PackFile {
    std::filesystem::path path;
    // Azahar's (a tex1_ name, or a pack.json's alias): its PNGs' first row
    // is the texture's first in memory (flip_png_files, the default; the
    // other way round without it), a DDS's or KTX's first row the last.
    // The runtime's own tex_ files go the other way for both
    bool azahar = false;
    bool flip_png = true;
};

// what a pack.json says, as Azahar reads it. a folder without one, which
// Azahar takes for a legacy pack (the legacy hash only, DDS passed over),
// has its names looked up here by either hash and its DDS read too
struct PackOptions {
    bool skip_mipmap = true, flip_png_files = true, use_new_hash = false;
    // hashes given to files by name (pack.json's "textures")
    std::map<uint64_t, std::vector<std::string>> aliases;
};
// pack.json's text read; false when it is not JSON with what Azahar needs
bool parse_pack_options(const std::string &json, PackOptions &out, std::string &error);

class TexturePacks {
public:
    // the files of a folder and every folder in it: tex1_ names by its
    // pack.json (or a legacy pack's rules), tex_ names the runtime's own. a
    // hash already given keeps its file; how many files were taken
    size_t add_folder(const std::filesystem::path &folder);
    // the file for a texture level in memory, or none. own: the runtime's
    // hash of it, for its own files
    const PackFile *find(uint32_t format, const uint8_t *bytes, uint32_t width, uint32_t height, uint64_t own) const;
    bool empty() const { return raw_.empty() && decoded_.empty() && own_.empty(); }
    size_t size() const { return raw_.size() + decoded_.size() + own_.size(); }
    // every file, for reading them all at once
    std::vector<const PackFile *> files() const;

private:
    std::map<uint64_t, PackFile> raw_, decoded_, own_;
};

// a dump's file name, as Azahar names its: tex1_<w>x<h>_<hash>_<format>_mip0.png
std::string dump_name(uint32_t width, uint32_t height, uint64_t hash, uint32_t format);
// a dump folder's pack.json, written when there is none, so the folder is a
// pack as it is: the new hash, PNGs as dumped, mips made from the base
void write_pack_template(const std::filesystem::path &folder);
// a title's folder in Azahar's layout: 16 hex digits, upper case
std::string title_folder(uint64_t program_id);

} // namespace runtime

#endif
