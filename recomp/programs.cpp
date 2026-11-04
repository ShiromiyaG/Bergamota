// A title's programs, the executable and its modules, each named and ready
// for discovery. Translated from 3dsrecomp's lib.rs (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-3dsrecomp).

#include "programs.h"

#include "image.h"

namespace recomp {

std::optional<std::vector<uint8_t>> Mods::read(const Title &title, const std::string &path,
                                               const FileEntry &file) const {
    if (romfs) {
        if (auto bytes = romfs(path)) return bytes;
    }
    return title.read_romfs(file, 0, size_t(file.data_size));
}

bool programs(const Title &title, const Mods &mods, std::vector<std::pair<std::string, Program>> &out,
              std::string &error) {
    Image image;
    if (mods.code) {
        std::optional<ExHeader> exheader;
        if (mods.exheader && mods.exheader->size() >= 0x400) exheader = ExHeader::read(*mods.exheader);
        image = Image::from_code(exheader ? *exheader : title.exheader, *mods.code);
    } else if (!Image::from_title(title, image, error)) {
        return false;
    }
    if (mods.decomp) mods.decomp->patch(image.text);
    std::vector<ModuleFile> files = module_files(title, mods);
    std::optional<cro::Module> crs = static_module(title, mods);
    const cro::Module *crs_module = crs ? &*crs : nullptr;
    std::vector<uint32_t> exports, imported_from_executable;
    if (crs) {
        exports = crs->code_exports();
        imported_from_executable = imported(files, crs_module, *crs);
    }
    std::vector<std::pair<std::string, Program>> list;
    list.emplace_back("executable", image.into_program(exports, imported_from_executable));
    for (const auto &[module, bytes] : files) {
        if (auto program = module.program(bytes, imported(files, crs_module, module)))
            list.emplace_back(module.name, std::move(*program));
    }
    // the module list is by name and in this order everywhere, so the code
    // written apart goes last
    if (mods.decomp) list.emplace_back(Decomp::NAME, mods.decomp->program());
    out = std::move(list);
    return true;
}

std::vector<uint32_t> imported(const std::vector<ModuleFile> &files, const cro::Module *crs,
                               const cro::Module &module) {
    std::vector<uint32_t> out;
    auto take = [&](const cro::Module &other) {
        for (const auto &[name, tag] : other.anonymous_imports) {
            if (name != module.name) continue;
            if (auto address = module.code_address(tag)) out.push_back(*address);
        }
    };
    for (const auto &[other, _] : files) take(other);
    if (crs) take(*crs);
    return out;
}

std::optional<cro::Module> static_module(const Title &title, const Mods &mods) {
    if (!title.romfs) return std::nullopt;
    std::string ignored;
    auto file = title.romfs->lookup("static.crs", ignored);
    if (!file) return std::nullopt;
    auto bytes = mods.read(title, "static.crs", *file);
    if (!bytes) return std::nullopt;
    return cro::parse(*bytes);
}

std::vector<ModuleFile> module_files(const Title &title, const Mods &mods) {
    if (!title.romfs) return {};
    const RomFs &romfs = *title.romfs;
    std::vector<std::pair<std::string, FileEntry>> found;
    std::string ignored;
    if (auto root = romfs.root(ignored)) find_modules(romfs, *root, "", found);
    std::vector<ModuleFile> files;
    for (const auto &[path, file] : found) {
        auto bytes = mods.read(title, path, file);
        if (!bytes) continue;
        auto module = cro::parse(*bytes);
        if (!module) continue;
        files.emplace_back(std::move(*module), std::move(*bytes));
    }
    return files;
}

void find_modules(const RomFs &romfs, const DirEntry &dir, const std::string &path,
                  std::vector<std::pair<std::string, FileEntry>> &files) {
    for (auto &[_, file] : romfs.files(dir)) {
        std::string lower = file.name;
        for (char &c : lower)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".cro") == 0)
            files.emplace_back(join(path, file.name), file);
    }
    for (const auto &[_, subdir] : romfs.subdirs(dir)) find_modules(romfs, subdir, join(path, subdir.name), files);
}

std::string join(const std::string &path, const std::string &name) {
    if (path.empty() || name.empty()) return path + name;
    return path + "/" + name;
}

} // namespace recomp
