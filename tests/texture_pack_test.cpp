// Texture packs without a game: CityHash64 as Google's v1.1 gives it (the
// vectors of docs/tools.md's description of Azahar's packs), Azahar's names,
// pack.json and a legacy pack's rules, and both keys of a texture.

#include "runtime/texture_pack.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "../src/pica/texture.h"

using namespace runtime;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

void city_hash_is_v1_1() {
    std::vector<uint8_t> bytes(65536);
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = uint8_t(i);
    const std::pair<size_t, uint64_t> vectors[] = {
        {0, 0x9AE16A3B2F90404Full},     {1, 0xBE6056EDF5E94B54ull},     {3, 0x94A13D22E9EBA49Aull},
        {8, 0xAD5A13E1E8E93B98ull},     {16, 0x0EFD25A0A34156D4ull},    {17, 0xBBB6A6F8F20D1F1Cull},
        {32, 0x1A9D8199972CDF49ull},    {33, 0x46E1378CBC22DABAull},    {64, 0xE99AB80F5EC7DCA5ull},
        {65, 0xAC589C990483DD2Eull},    {100, 0xA47FE83E60B34CC6ull},   {256, 0x0C693049D8C2C68Dull},
        {4096, 0x9ADDE01425F78614ull},  {32768, 0x3E787211A4A689D2ull}, {65536, 0xE7F4C4C9A51C0B6Bull},
    };
    for (auto [size, hash] : vectors) {
        CHECK(city_hash64(bytes.data(), size) == hash);
        if (city_hash64(bytes.data(), size) != hash) std::printf("  length %zu\n", size);
    }
    std::vector<uint8_t> zeros(65536, 0);
    CHECK(city_hash64(zeros.data(), 32) == 0x7087061603E53293ull);
    CHECK(city_hash64(zeros.data(), 2048) == 0xD590E1A5E878375Eull);
    CHECK(city_hash64(zeros.data(), 65536) == 0xE0C05D55F4D88F16ull);
}

uint32_t z_order(uint32_t x, uint32_t y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

// an 8x8 texture whose texels say where they are, in memory's tiles
std::vector<uint8_t> marked(uint32_t bytes_per_texel) {
    std::vector<uint8_t> t(64 * bytes_per_texel);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++)
            for (uint32_t b = 0; b < bytes_per_texel; b++) t[z_order(x, y) * bytes_per_texel + b] = uint8_t(y * 8 + x + b * 64);
    return t;
}

void both_keys() {
    // the new one: the bytes as they are
    std::vector<uint8_t> rgba8 = marked(4);
    CHECK(raw_texture_hash(rgba8.data(), rgba8.size()) == city_hash64(rgba8.data(), rgba8.size()));
    // the legacy one, a colour format: its texels' own bytes out of the tile,
    // the last row in memory first
    std::vector<uint8_t> want(64 * 4);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++)
            for (uint32_t b = 0; b < 4; b++) want[((7 - y) * 8 + x) * 4 + b] = uint8_t(y * 8 + x + b * 64);
    CHECK(decoded_texture_hash(0, rgba8.data(), 8, 8) == city_hash64(want.data(), want.size()));
    // an A8: R, G, B, A as 0, 0, 0, A, the rows the same way round
    std::vector<uint8_t> a8 = marked(1), want_a8(64 * 4, 0);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) want_a8[((7 - y) * 8 + x) * 4 + 3] = uint8_t(y * 8 + x);
    CHECK(decoded_texture_hash(8, a8.data(), 8, 8) == city_hash64(want_a8.data(), want_a8.size()));
}

void pack_json() {
    PackOptions o;
    std::string error;
    CHECK(parse_pack_options("// a pack\n{ \"author\": \"x\", /* the options */ \"options\": { \"skip_mipmap\": true, "
                             "\"flip_png_files\": false, \"use_new_hash\": true },\n \"textures\": { \"114bfc385ed72f15\": "
                             "\"sub/logo.png\", \"0x2\": [\"a.png\", \"b.dds\"] } }",
                             o, error));
    CHECK(o.skip_mipmap && !o.flip_png_files && o.use_new_hash);
    CHECK(o.aliases.size() == 2 && o.aliases[0x114BFC385ED72F15ull].size() == 1 && o.aliases[0x114BFC385ED72F15ull][0] == "logo.png");
    CHECK(o.aliases[2].size() == 2 && o.aliases[2][1] == "b.dds");
    // the template's values where one is left out
    PackOptions t;
    CHECK(parse_pack_options("{\"options\": {}}", t, error) && !t.skip_mipmap && t.flip_png_files && t.use_new_hash);
    CHECK(!parse_pack_options("{\"options\": ", t, error));
}

void touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "x";
}

void folders() {
    std::vector<uint8_t> tex = marked(4);
    const uint64_t raw = raw_texture_hash(tex.data(), tex.size()), legacy = decoded_texture_hash(0, tex.data(), 8, 8);
    char name[96];

    // a pack with a pack.json: the new hash, its DDS taken (skip_mipmap off),
    // names in subfolders, normal maps left out
    fs::path a = fs::temp_directory_path() / "runtime-texture-pack-a";
    fs::remove_all(a);
    fs::create_directories(a);
    std::ofstream(a / "pack.json") << "{\"options\": {\"skip_mipmap\": false, \"flip_png_files\": true, \"use_new_hash\": true}}";
    std::snprintf(name, sizeof name, "sub/tex1_8x8_%016llX_0_mip0.dds", (unsigned long long)raw);
    touch(a / name);
    std::snprintf(name, sizeof name, "tex1_8x8_%016llX_0.norm.png", (unsigned long long)(raw + 1));
    touch(a / name);
    TexturePacks packs;
    CHECK(packs.add_folder(a) == 1);
    const PackFile *found = packs.find(0, tex.data(), 8, 8, 0);
    CHECK(found && found->azahar && found->path.filename().string().find(".dds") != std::string::npos);

    // a legacy pack (no pack.json): the decoded hash, DDS passed over, a PNG
    // taken; and the runtime's own tex_ names with its own hash
    fs::path b = fs::temp_directory_path() / "runtime-texture-pack-b";
    fs::remove_all(b);
    fs::create_directories(b);
    std::snprintf(name, sizeof name, "tex1_8x8_%016llx_0.png", (unsigned long long)legacy);
    touch(b / name);
    std::snprintf(name, sizeof name, "tex1_8x8_%016llX_0.dds", (unsigned long long)(legacy ^ 1));
    touch(b / name);
    touch(b / "tex_8x8_00000000000000AB_0.png");
    TexturePacks legacy_packs;
    CHECK(legacy_packs.add_folder(b) == 2);
    const PackFile *png = legacy_packs.find(0, tex.data(), 8, 8, 0);
    CHECK(png && png->azahar && png->flip_png);
    const PackFile *own = legacy_packs.find(0, tex.data(), 8, 8, 0xAB);
    CHECK(own && (own == png || !own->azahar));
    std::vector<uint8_t> other(tex.size(), 7);
    const PackFile *own_only = legacy_packs.find(0, other.data(), 8, 8, 0xAB);
    CHECK(own_only && !own_only->azahar);
    CHECK(!legacy_packs.find(0, other.data(), 8, 8, 0xAC));

    // the first folder to give a hash keeps it
    TexturePacks both;
    both.add_folder(a);
    both.add_folder(b);
    CHECK(both.find(0, tex.data(), 8, 8, 0) == both.find(0, tex.data(), 8, 8, 0));
    fs::remove_all(a);
    fs::remove_all(b);
}

void names() {
    CHECK(dump_name(256, 128, 0x543624189C94B105ull, 12) == "tex1_256x128_543624189C94B105_12_mip0.png");
    CHECK(title_folder(0x00040000001B5000ull) == "00040000001B5000");
    fs::path d = fs::temp_directory_path() / "runtime-texture-pack-dump";
    fs::remove_all(d);
    fs::create_directories(d);
    write_pack_template(d);
    PackOptions o;
    std::string error, text;
    std::ifstream in(d / "pack.json");
    text.assign(std::istreambuf_iterator<char>(in), {});
    CHECK(parse_pack_options(text, o, error) && o.use_new_hash && o.flip_png_files && o.skip_mipmap);
    in.close();
    fs::remove_all(d);
}

} // namespace

int main() {
    city_hash_is_v1_1();
    both_keys();
    pack_json();
    folders();
    names();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
