// The bits of Rust's standard library the front end's translation leans on,
// so that what it accepts and what it says match 3dsrecomp's: lossy UTF-8,
// Unicode whitespace, str::lines, integer parsing, and reading files with
// io::Error's messages (os_error in rust.cpp). Written for the translation
// of 3dsrecomp (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).

#ifndef RECOMP_RUST_H
#define RECOMP_RUST_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <share.h>
#endif

namespace recomp::rust {

// how far the valid UTF-8 at the start of bytes goes, the way core's
// Utf8Chunks scans it; bad is how many bytes the invalid sequence after it
// has (0 at the end)
inline size_t utf8_valid_up_to(const uint8_t *bytes, size_t size, size_t &bad) {
    auto get = [&](size_t at) -> uint8_t { return at < size ? bytes[at] : 0; };
    auto cont = [](uint8_t b) { return (b & 0xC0) == 0x80; };
    size_t i = 0, valid = 0;
    bad = 0;
    while (i < size) {
        uint8_t b = bytes[i++];
        if (b >= 0x80) {
            bool ok = true;
            if (b >= 0xC2 && b <= 0xDF) {
                if (!cont(get(i))) ok = false;
                else i++;
            } else if (b >= 0xE0 && b <= 0xEF) {
                uint8_t c = get(i);
                bool lead = (b == 0xE0 && c >= 0xA0 && c <= 0xBF) || (b >= 0xE1 && b <= 0xEC && cont(c)) ||
                            (b == 0xED && c >= 0x80 && c <= 0x9F) || (b >= 0xEE && cont(c));
                if (!lead) ok = false;
                else if (!cont(get(++i))) ok = false;
                else i++;
            } else if (b >= 0xF0 && b <= 0xF4) {
                uint8_t c = get(i);
                bool lead = (b == 0xF0 && c >= 0x90 && c <= 0xBF) || (b >= 0xF1 && b <= 0xF3 && cont(c)) ||
                            (b == 0xF4 && c >= 0x80 && c <= 0x8F);
                if (!lead) ok = false;
                else if (!cont(get(++i))) ok = false;
                else if (!cont(get(++i))) ok = false;
                else i++;
            } else {
                ok = false;
            }
            if (!ok) {
                bad = i - valid;
                return valid;
            }
        }
        valid = i;
    }
    return valid;
}

// String::from_utf8_lossy: each invalid sequence becomes one U+FFFD
inline std::string from_utf8_lossy(const uint8_t *bytes, size_t size) {
    std::string out;
    out.reserve(size);
    while (size) {
        size_t bad = 0;
        size_t valid = utf8_valid_up_to(bytes, size, bad);
        out.append(reinterpret_cast<const char *>(bytes), valid);
        if (bad) out += "\xEF\xBF\xBD";
        bytes += valid + bad;
        size -= valid + bad;
    }
    return out;
}

inline std::string from_utf8_lossy(const std::vector<uint8_t> &bytes) {
    return from_utf8_lossy(bytes.data(), bytes.size());
}

// whether str::from_utf8 takes it
inline bool is_utf8(std::string_view text) {
    size_t bad = 0;
    return utf8_valid_up_to(reinterpret_cast<const uint8_t *>(text.data()), text.size(), bad) == text.size();
}

// a code point as UTF-8
inline void push_char(std::string &out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// the length of the char a byte starts, in a valid string
inline size_t char_size(uint8_t lead) { return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4; }

// the char at text[at], and its length in size
inline uint32_t char_at(std::string_view text, size_t at, size_t &size) {
    auto b = [&](size_t i) { return uint32_t(uint8_t(text[i])); };
    size = char_size(uint8_t(text[at]));
    if (at + size > text.size()) size = text.size() - at;
    switch (size) {
    case 1: return b(at);
    case 2: return (b(at) & 0x1F) << 6 | (b(at + 1) & 0x3F);
    case 3: return (b(at) & 0x0F) << 12 | (b(at + 1) & 0x3F) << 6 | (b(at + 2) & 0x3F);
    default: return (b(at) & 0x07) << 18 | (b(at + 1) & 0x3F) << 12 | (b(at + 2) & 0x3F) << 6 | (b(at + 3) & 0x3F);
    }
}

// the char that ends at text[end - 1], and where it starts
inline uint32_t char_before(std::string_view text, size_t end, size_t &start) {
    start = end - 1;
    while (start > 0 && (uint8_t(text[start]) & 0xC0) == 0x80 && end - start < 4) start--;
    size_t size;
    return char_at(text, start, size);
}

// char::is_whitespace, Unicode's White_Space
inline bool is_whitespace(uint32_t c) {
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return true;
    if (c <= 0x7F) return false;
    return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

inline std::string_view trim_start(std::string_view text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t size;
        if (!is_whitespace(char_at(text, at, size))) break;
        at += size;
    }
    return text.substr(at);
}

inline std::string_view trim_end(std::string_view text) {
    size_t end = text.size();
    while (end > 0) {
        size_t start;
        if (!is_whitespace(char_before(text, end, start))) break;
        end = start;
    }
    return text.substr(0, end);
}

inline std::string_view trim(std::string_view text) { return trim_end(trim_start(text)); }

// str::split_whitespace: the pieces between runs of whitespace
inline std::vector<std::string_view> split_whitespace(std::string_view text) {
    std::vector<std::string_view> out;
    size_t at = 0, start = std::string_view::npos;
    while (at < text.size()) {
        size_t size;
        bool space = is_whitespace(char_at(text, at, size));
        if (space && start != std::string_view::npos) {
            out.push_back(text.substr(start, at - start));
            start = std::string_view::npos;
        } else if (!space && start == std::string_view::npos) {
            start = at;
        }
        at += size;
    }
    if (start != std::string_view::npos) out.push_back(text.substr(start));
    return out;
}

// str::lines: split after each \n, which goes with a \r before it, and no
// empty line after the last one
inline std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        if (newline == std::string_view::npos) {
            text = {};
        } else {
            text.remove_prefix(newline + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        }
        out.push_back(line);
    }
    return out;
}

// u32::from_str_radix and str::parse for the unsigned types: an optional
// +, then digits, nothing over max
inline std::optional<uint64_t> parse_unsigned(std::string_view text, unsigned radix, uint64_t max) {
    if (text.empty()) return std::nullopt;
    if (text[0] == '+') {
        if (text.size() == 1) return std::nullopt;
        text.remove_prefix(1);
    }
    uint64_t value = 0;
    for (char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = unsigned(c - '0');
        else if (c >= 'a' && c <= 'z') digit = unsigned(c - 'a') + 10;
        else if (c >= 'A' && c <= 'Z') digit = unsigned(c - 'A') + 10;
        else return std::nullopt;
        if (digit >= radix) return std::nullopt;
        if (value > (max - digit) / radix) return std::nullopt;
        value = value * radix + digit;
    }
    return value;
}

inline std::optional<uint32_t> parse_u32(std::string_view text, unsigned radix) {
    auto value = parse_unsigned(text, radix, UINT32_MAX);
    if (!value) return std::nullopt;
    return uint32_t(*value);
}

// Path::display
inline std::string display(const std::filesystem::path &path) {
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// io::Error's message for an error the system gave, as Display writes it:
// the system's text, in its language, and the code (rust.cpp)
std::string os_error(int code);

// File::open for reading; null with io::Error's message
inline std::FILE *open(const std::filesystem::path &path, std::string &error) {
#ifdef _WIN32
    _doserrno = 0;
    std::FILE *file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!file) error = os_error(_doserrno ? int(_doserrno) : errno);
#else
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) error = os_error(errno);
#endif
    return file;
}

// a seek to offset from the start, 64 bits wide
inline bool seek(std::FILE *file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, int64_t(offset), SEEK_SET) == 0;
#else
    return fseeko(file, off_t(offset), SEEK_SET) == 0;
#endif
}

// std::fs::read; false with io::Error's message
inline bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &out, std::string &error) {
    std::FILE *file = open(path, error);
    if (!file) return false;
    out.clear();
    uint8_t buffer[1 << 16];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) out.insert(out.end(), buffer, buffer + got);
    bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        error = os_error(EIO);
        return false;
    }
    return true;
}

// std::fs::read_to_string, which refuses what is not UTF-8
inline bool read_to_string(const std::filesystem::path &path, std::string &out, std::string &error) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes, error)) return false;
    out.assign(bytes.begin(), bytes.end());
    if (!is_utf8(out)) {
        error = "stream did not contain valid UTF-8";
        return false;
    }
    return true;
}

} // namespace recomp::rust

#endif
