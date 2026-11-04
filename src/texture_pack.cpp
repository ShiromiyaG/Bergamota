// Texture packs (texture_pack.h): Azahar's, from how they behave, and the
// runtime's own.

#include "runtime/texture_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <city.h>

#include "pica/texture.h"

namespace fs = std::filesystem;

namespace runtime {

uint64_t city_hash64(const void *data, size_t size) { return CityHash64(static_cast<const char *>(data), size); }

uint64_t raw_texture_hash(const uint8_t *bytes, size_t size) { return city_hash64(bytes, size); }

namespace {

uint32_t z_order(uint32_t x, uint32_t y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string utf8(const fs::path &path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}

// a hex number at the start of text (an optional 0x, either case), as a
// scanf's %llX reads it; how many characters it took, 0 for none
size_t read_hex(const std::string &text, size_t at, uint64_t &out) {
    size_t i = at;
    if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
    size_t start = i;
    uint64_t value = 0;
    while (i < text.size() && std::isxdigit(uint8_t(text[i]))) {
        char c = text[i++];
        value = value << 4 | uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    if (i == start) return 0;
    out = value;
    return i - at;
}

size_t read_decimal(const std::string &text, size_t at, uint32_t &out) {
    size_t i = at;
    uint64_t value = 0;
    while (i < text.size() && std::isdigit(uint8_t(text[i]))) value = value * 10 + uint64_t(text[i++] - '0');
    out = uint32_t(value);
    return i - at;
}

// tex1_<w>x<h>_<hash>_<format>, anything after: the hash
std::optional<uint64_t> azahar_hash(const std::string &base) {
    if (base.rfind("tex1_", 0) != 0) return std::nullopt;
    size_t at = 5;
    uint32_t number;
    uint64_t hash;
    size_t n;
    if (!(n = read_decimal(base, at, number)) || base.size() <= at + n || base[at + n] != 'x') return std::nullopt;
    at += n + 1;
    if (!(n = read_decimal(base, at, number)) || base.size() <= at + n || base[at + n] != '_') return std::nullopt;
    at += n + 1;
    if (!(n = read_hex(base, at, hash)) || base.size() <= at + n || base[at + n] != '_') return std::nullopt;
    at += n + 1;
    if (!read_decimal(base, at, number)) return std::nullopt;
    return hash;
}

// the runtime's own names: 16 hex digits on their own anywhere in it
std::optional<uint64_t> own_hash(const std::string &stem) {
    auto hex = [&](size_t k) { return k < stem.size() && std::isxdigit(uint8_t(stem[k])); };
    for (size_t i = 0; i + 16 <= stem.size(); i++) {
        if ((i && hex(i - 1)) || hex(i + 16)) continue;
        bool all = true;
        for (size_t k = i; k < i + 16 && all; k++) all = hex(k);
        if (all) return std::stoull(stem.substr(i, 16), nullptr, 16);
    }
    return std::nullopt;
}

// just enough JSON for pack.json: objects, arrays, strings, booleans,
// numbers and null, with // and /* */ comments, as Azahar's reader takes
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json *get(const std::string &key) const {
        for (const auto &[k, v] : members) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string &s) : s_(s) {}
    bool parse(Json &out) {
        if (!value(out)) return false;
        space();
        return at_ == s_.size();
    }

private:
    const std::string &s_;
    size_t at_ = 0;

    void space() {
        for (;;) {
            while (at_ < s_.size() && std::isspace(uint8_t(s_[at_]))) at_++;
            if (s_.compare(at_, 2, "//") == 0) {
                while (at_ < s_.size() && s_[at_] != '\n') at_++;
            } else if (s_.compare(at_, 2, "/*") == 0) {
                size_t end = s_.find("*/", at_ + 2);
                at_ = end == std::string::npos ? s_.size() : end + 2;
            } else {
                return;
            }
        }
    }
    bool string(std::string &out) {
        if (at_ >= s_.size() || s_[at_] != '"') return false;
        at_++;
        while (at_ < s_.size() && s_[at_] != '"') {
            char c = s_[at_++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) return false;
            char e = s_[at_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (at_ + 4 > s_.size()) return false;
                unsigned code = unsigned(std::stoul(s_.substr(at_, 4), nullptr, 16));
                at_ += 4;
                // what file names need: as UTF-8
                if (code < 0x80) out += char(code);
                else if (code < 0x800) out += char(0xC0 | code >> 6), out += char(0x80 | (code & 63));
                else out += char(0xE0 | code >> 12), out += char(0x80 | (code >> 6 & 63)), out += char(0x80 | (code & 63));
                break;
            }
            default: out += e;
            }
        }
        if (at_ >= s_.size()) return false;
        at_++;
        return true;
    }
    bool value(Json &out) {
        space();
        if (at_ >= s_.size()) return false;
        char c = s_[at_];
        if (c == '{') {
            out.kind = Json::Object;
            at_++;
            space();
            if (at_ < s_.size() && s_[at_] == '}') return at_++, true;
            for (;;) {
                space();
                std::string key;
                if (!string(key)) return false;
                space();
                if (at_ >= s_.size() || s_[at_++] != ':') return false;
                Json v;
                if (!value(v)) return false;
                out.members.emplace_back(std::move(key), std::move(v));
                space();
                if (at_ < s_.size() && s_[at_] == ',') {
                    at_++;
                    continue;
                }
                return at_ < s_.size() && s_[at_++] == '}';
            }
        }
        if (c == '[') {
            out.kind = Json::Array;
            at_++;
            space();
            if (at_ < s_.size() && s_[at_] == ']') return at_++, true;
            for (;;) {
                Json v;
                if (!value(v)) return false;
                out.items.push_back(std::move(v));
                space();
                if (at_ < s_.size() && s_[at_] == ',') {
                    at_++;
                    continue;
                }
                return at_ < s_.size() && s_[at_++] == ']';
            }
        }
        if (c == '"') {
            out.kind = Json::String;
            return string(out.text);
        }
        for (const char *word : {"true", "false", "null"}) {
            if (s_.compare(at_, std::strlen(word), word) == 0) {
                at_ += std::strlen(word);
                out.kind = word[0] == 'n' ? Json::Null : Json::Bool;
                out.boolean = word[0] == 't';
                return true;
            }
        }
        size_t start = at_;
        while (at_ < s_.size() && (std::isdigit(uint8_t(s_[at_])) || std::strchr("+-.eE", s_[at_]))) at_++;
        if (at_ == start) return false;
        out.kind = Json::Number;
        out.text = s_.substr(start, at_ - start);
        return true;
    }
};

std::string leaf_of(const std::string &name) {
    size_t slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

} // namespace

uint64_t decoded_texture_hash(uint32_t format, const uint8_t *bytes, uint32_t width, uint32_t height) {
    format &= 15;
    std::vector<uint8_t> buffer;
    if (format <= 4) {
        // the colour formats keep their texels' bytes, only out of the tiles
        static constexpr uint32_t BYTES[5] = {4, 3, 2, 2, 2};
        const uint32_t n = BYTES[format];
        buffer.resize(size_t(width) * height * n);
        for (uint32_t y = 0; y < height; y++) {
            uint8_t *row = buffer.data() + size_t(height - 1 - y) * width * n;
            for (uint32_t x = 0; x < width; x++) {
                uint32_t texel = (y / 8 * (width / 8) + x / 8) * 64 + z_order(x, y);
                std::memcpy(row + size_t(x) * n, bytes + size_t(texel) * n, n);
            }
        }
    } else {
        // the others as R, G, B, A, rows the other way round
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        pica::decode_texture(format, bytes, width, height, rgba.data());
        buffer.resize(rgba.size());
        for (uint32_t y = 0; y < height; y++)
            std::memcpy(&buffer[size_t(height - 1 - y) * width * 4], &rgba[size_t(y) * width * 4], size_t(width) * 4);
    }
    return city_hash64(buffer.data(), buffer.size());
}

bool parse_pack_options(const std::string &json, PackOptions &out, std::string &error) {
    Json root;
    if (!Parser(json).parse(root) || root.kind != Json::Object) {
        error = "pack.json is not JSON";
        return false;
    }
    // a working Azahar pack has all three; one left out takes the template's
    PackOptions options;
    options.skip_mipmap = false, options.flip_png_files = true, options.use_new_hash = true;
    if (const Json *o = root.get("options"); o && o->kind == Json::Object) {
        if (const Json *v = o->get("skip_mipmap"); v && v->kind == Json::Bool) options.skip_mipmap = v->boolean;
        if (const Json *v = o->get("flip_png_files"); v && v->kind == Json::Bool) options.flip_png_files = v->boolean;
        if (const Json *v = o->get("use_new_hash"); v && v->kind == Json::Bool) options.use_new_hash = v->boolean;
    }
    if (const Json *t = root.get("textures"); t && t->kind == Json::Object) {
        for (const auto &[key, value] : t->members) {
            size_t at = key.find_first_not_of(" \t");
            uint64_t hash;
            if (at == std::string::npos || !read_hex(key, at, hash)) continue;
            auto &names = options.aliases[hash];
            if (value.kind == Json::String) names.push_back(leaf_of(value.text));
            for (const Json &item : value.items) {
                if (item.kind == Json::String) names.push_back(leaf_of(item.text));
            }
        }
    }
    out = std::move(options);
    return true;
}

size_t TexturePacks::add_folder(const fs::path &folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return 0;
    // Azahar's pack.json, at the folder's top only; without one a legacy pack
    PackOptions options;
    bool has_json = false;
    if (std::ifstream in(folder / "pack.json", std::ios::binary); in) {
        std::string text((std::istreambuf_iterator<char>(in)), {}), error;
        if (parse_pack_options(text, options, error)) has_json = true;
        else std::fprintf(stderr, "runtime: %s: %s, read as a pack without one\n", utf8(folder / "pack.json").c_str(), error.c_str());
    }
    // every file, in an order of their own (as Windows lists them): the first
    // with a hash keeps it
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return lower(utf8(a)) < lower(utf8(b)); });
    size_t taken = 0;
    auto give = [&](std::map<uint64_t, PackFile> &to, uint64_t hash, const fs::path &path, bool is_azahar) {
        return to.try_emplace(hash, PackFile{path, is_azahar, options.flip_png_files, has_json}).second;
    };
    // an Azahar file: by its pack.json's hash; without one by either (Azahar
    // takes the legacy one alone there; a tool that names files by the new
    // one, as dumps from a pack.json are, then works too, and the two never
    // meet: 64 bits each)
    auto give_azahar = [&](uint64_t hash, const fs::path &path) {
        bool given = false;
        if (!has_json || options.use_new_hash) given |= give(raw_, hash, path, true);
        if (!has_json || !options.use_new_hash) given |= give(decoded_, hash, path, true);
        return given;
    };
    // the files the aliases name, by their leaf name
    std::map<std::string, std::vector<uint64_t>> aliased;
    for (const auto &[hash, names] : options.aliases) {
        for (const std::string &name : names) aliased[name].push_back(hash);
    }
    for (const fs::path &path : files) {
        const std::string leaf = utf8(path.filename());
        std::vector<std::string> parts;
        for (size_t at = 0;;) {
            size_t dot = leaf.find('.', at);
            parts.push_back(leaf.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
            if (dot == std::string::npos) break;
            at = dot + 1;
        }
        if (parts.size() < 2 || parts.size() > 3) continue;
        const std::string ext = lower(parts.back());
        if (ext != "png" && ext != "dds" && ext != "ktx") continue;
        // normal maps (name.norm.png) are not drawn here
        if (parts.size() == 3 && lower(parts[1]) == "norm") continue;
        // where a pack.json has mips made from the base (skip_mipmap),
        // Azahar passes a DDS over; a folder without one, which Azahar
        // takes for a legacy pack and passes every DDS over in, has its
        // DDS read here (the runtime draws the first level only)
        const bool dds_out = ext == "dds" && has_json && options.skip_mipmap;
        bool given = false;
        if (auto alias = aliased.find(leaf); alias != aliased.end() && !dds_out) {
            for (uint64_t hash : alias->second) given |= give_azahar(hash, path);
        }
        if (auto hash = azahar_hash(parts[0])) {
            if (!dds_out) given |= give_azahar(*hash, path);
        } else if (parts[0].rfind("tex1_", 0) != 0) {
            if (auto own = own_hash(parts[0])) given |= give(own_, *own, path, false);
        }
        taken += given;
    }
    return taken;
}

const PackFile *TexturePacks::find(uint32_t format, const uint8_t *bytes, uint32_t width, uint32_t height, uint64_t own) const {
    if (!raw_.empty()) {
        size_t size = size_t(width) * height * pica::texture_bits(format) / 8;
        if (auto it = raw_.find(raw_texture_hash(bytes, size)); it != raw_.end()) return &it->second;
    }
    if (!decoded_.empty()) {
        if (auto it = decoded_.find(decoded_texture_hash(format, bytes, width, height)); it != decoded_.end()) return &it->second;
    }
    if (auto it = own_.find(own); it != own_.end()) return &it->second;
    return nullptr;
}

std::vector<const PackFile *> TexturePacks::files() const {
    std::vector<const PackFile *> out;
    for (const auto *map : {&raw_, &decoded_, &own_}) {
        for (const auto &[hash, file] : *map) out.push_back(&file);
    }
    return out;
}

std::string dump_name(uint32_t width, uint32_t height, uint64_t hash, uint32_t format) {
    char name[96];
    std::snprintf(name, sizeof name, "tex1_%ux%u_%016llX_%u_mip0.png", width, height, (unsigned long long)hash, format);
    return name;
}

void write_pack_template(const fs::path &folder) {
    std::error_code ec;
    if (fs::exists(folder / "pack.json", ec)) return;
    std::ofstream out(folder / "pack.json", std::ios::binary);
    out << "{\n"
           "    \"author\": \"\",\n"
           "    \"version\": \"1.0.0\",\n"
           "    \"description\": \"Textures dumped by the runtime, in Azahar's layout\",\n"
           "    \"options\": {\n"
           "        \"skip_mipmap\": true,\n"
           "        \"flip_png_files\": true,\n"
           "        \"use_new_hash\": true\n"
           "    }\n"
           "}\n";
}

std::string title_folder(uint64_t program_id) {
    char name[24];
    std::snprintf(name, sizeof name, "%016llX", (unsigned long long)program_id);
    return name;
}

} // namespace runtime
