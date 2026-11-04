// Functions written by hand to run instead of the ones the recompiler
// generates, found in C files by the RECOMP_OVERRIDE marks recomp.h gives
// them. Translated from 3dsrecomp's overrides.rs (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-3dsrecomp).

#include "overrides.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <system_error>

#include "rust.h"

namespace recomp {

namespace {

bool is_identifier(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool all_identifier(std::string_view text) { return std::all_of(text.begin(), text.end(), is_identifier); }

// a literal as C writes it, which also has to fit in a name
bool number(std::string_view literal, uint32_t &value, std::string &error) {
    std::optional<uint32_t> parsed;
    if (literal.substr(0, 2) == "0x" || literal.substr(0, 2) == "0X") parsed = rust::parse_u32(literal.substr(2), 16);
    else parsed = rust::parse_u32(literal, 10);
    if (parsed && all_identifier(literal)) {
        value = *parsed;
        return true;
    }
    error = std::string(literal) + " is not an address";
    return false;
}

// the source with its comments and the insides of its strings blanked out
std::string strip_comments(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    size_t at = 0;
    // the next char, as its bytes
    auto next = [&](std::string_view &c) {
        if (at >= source.size()) return false;
        size_t size = std::min(rust::char_size(uint8_t(source[at])), source.size() - at);
        c = source.substr(at, size);
        at += size;
        return true;
    };
    auto peek = [&](char c) { return at < source.size() && source[at] == c; };
    std::string_view c;
    while (next(c)) {
        if (c == "/" && peek('/')) {
            std::string_view d;
            while (next(d)) {
                if (d == "\n") {
                    out += '\n';
                    break;
                }
            }
        } else if (c == "/" && peek('*')) {
            std::string_view d, last = " ";
            next(d);
            while (next(d)) {
                if (last == "*" && d == "/") break;
                last = d;
            }
            out += ' ';
        } else if (c == "\"" || c == "'") {
            out += c;
            std::string_view inner;
            while (next(inner)) {
                if (inner == "\\") {
                    std::string_view skipped;
                    next(skipped);
                    out += "  ";
                } else if (inner == c) {
                    out += c;
                    break;
                } else {
                    out += ' ';
                }
            }
        } else {
            out += c;
        }
    }
    return out;
}

std::string hex8(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "%08X", value);
    return text;
}

} // namespace

bool load_overrides(const std::filesystem::path &path, std::vector<OverrideFile> &out, std::string &error) {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::filesystem::directory_iterator entries(path, ec);
        if (ec) {
            error = "could not read " + rust::display(path) + ", " + rust::os_error(ec.value());
            return false;
        }
        for (; entries != std::filesystem::directory_iterator(); entries.increment(ec)) {
            if (ec) break;
            std::filesystem::path entry = entries->path();
            if (entry.extension() == ".c") paths.push_back(entry);
        }
        // the Rust sorts the paths by their components, which in one folder
        // is by the names' bytes
        std::sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) {
            return a.filename().u8string() < b.filename().u8string();
        });
    } else {
        paths.push_back(path);
    }
    std::vector<OverrideFile> files;
    for (const auto &file : paths) {
        OverrideFile item;
        item.path = file;
        std::string why;
        if (!rust::read_to_string(file, item.source, why)) {
            error = "could not read " + rust::display(file) + ", " + why;
            return false;
        }
        if (!parse_overrides(item.source, item.overrides, why)) {
            error = rust::display(file) + ", " + why;
            return false;
        }
        files.push_back(std::move(item));
    }
    out = std::move(files);
    return true;
}

bool parse_overrides(const std::string &source, std::vector<Override> &out, std::string &error) {
    constexpr std::string_view MARK = "RECOMP_OVERRIDE";
    std::string code = strip_comments(source);
    std::vector<Override> overrides;
    std::string_view rest = code;
    size_t at;
    while ((at = rest.find(MARK)) != std::string_view::npos) {
        // the char before it, whose last byte says whether it is one of an
        // identifier's
        bool joined = at > 0 && is_identifier(rest[at - 1]);
        rest = rest.substr(at + MARK.size());
        if (joined) continue;
        bool in_module = rest.substr(0, 3) == "_IN";
        std::string_view after = rust::trim_start(in_module ? rest.substr(3) : rest);
        if (after.empty() || after[0] != '(') continue;
        after.remove_prefix(1);
        size_t end = after.find(')');
        if (end == std::string_view::npos) {
            error = "a RECOMP_OVERRIDE is not closed";
            return false;
        }
        std::vector<std::string_view> arguments;
        std::string_view list = after.substr(0, end);
        while (true) {
            size_t comma = list.find(',');
            arguments.push_back(rust::trim(list.substr(0, comma)));
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        rest = after.substr(end);
        Override item;
        if (!in_module && arguments.size() == 1) {
            std::string_view literal = arguments[0];
            if (!number(literal, item.address, error)) return false;
            item.name = "override_" + std::string(literal);
            item.original = "original_" + std::string(literal);
        } else if (in_module && arguments.size() == 2) {
            std::string_view module = arguments[0], literal = arguments[1];
            if (module.empty() || !all_identifier(module)) {
                error = std::string(module) + " is not a module name";
                return false;
            }
            item.module = std::string(module);
            if (!number(literal, item.address, error)) return false;
            item.name = "override_" + std::string(module) + "_" + std::string(literal);
            item.original = "original_" + std::string(module) + "_" + std::string(literal);
        } else if (in_module) {
            error = "RECOMP_OVERRIDE_IN takes a module and an offset";
            return false;
        } else {
            error = "RECOMP_OVERRIDE takes an address";
            return false;
        }
        for (const Override &o : overrides) {
            if (o.module == item.module && o.address == item.address) {
                error = "0x" + hex8(item.address) + " is overridden twice";
                return false;
            }
        }
        overrides.push_back(std::move(item));
    }
    out = std::move(overrides);
    return true;
}

} // namespace recomp
