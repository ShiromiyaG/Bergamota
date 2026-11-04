// A game opened on the runtime (session.h).

#include "runtime/session.h"

#include <cstdio>
#include <fstream>
#include <iterator>

#include "runtime/cheats.h"

namespace runtime {

namespace {

// the session open: System's plain functions reach it here
Session *open_session = nullptr;

Code lookup(uint32_t address) { return open_session->library.lookup(address); }
uint32_t redirect(uint32_t address) { return open_session->library.redirect(address); }
void serve(System &system, Thread &thread, const std::string &service, uint32_t subhandle) {
    open_session->services->request(system, thread, service, subhandle);
}

// paths are UTF-8, as the settings keep them
std::filesystem::path path_of(const std::string &utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream in(path_of(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

Session::Session() = default;

CheatRun Session::run_cheats(const std::vector<bool> &on, uint32_t buttons) {
    CheatRun run;
    uint32_t text = exheader.text.address, text_end = text + exheader.text.size;
    for (size_t i = 0; i < cheats.size() && i < on.size(); i++) {
        if (on[i]) run_cheat(cheats[i], system->memory, buttons, text, text_end, run);
    }
    // a cheat that changed the code: those functions to the interpreter
    if (run.code) {
        if (size_t stale = library.check(0, 0, system->memory)) {
            std::printf("cheats: %zu functions of the executable run in the interpreter\n", stale);
        }
    }
    return run;
}

Session::~Session() {
    // the system first, while what its threads reach is still there
    system.reset();
    if (open_session == this) open_session = nullptr;
}

bool Session::open(const Files &files, Config config, std::string &error,
                   std::function<void(const std::string &, uint32_t)> on_module) {
    // the program: from the files named, else from the dump
    std::vector<uint8_t> exheader_bytes, code;
    if (!files.exheader.empty() && !files.code.empty()) {
        exheader_bytes = read_file(files.exheader);
        code = read_file(files.code);
    } else if (!read_program(path_of(files.update.empty() ? files.game : files.update), exheader_bytes,
                             code, error)) {
        return false;
    }
    if (!parse_exheader(exheader_bytes, exheader, error)) return false;
    // code the exheader says is compressed and is shorter than the program
    // it lays out (ctrtool without --decompresscode leaves it so): made whole
    const size_t whole = size_t(exheader.text.pages + exheader.rodata.pages) * 0x1000 + exheader.data.size;
    if (exheader.compressed && code.size() < whole && !blz_decompress(code, error)) return false;
    exheader.compressed = false;
    // the player's own changes: the program patched as it loads (what that
    // changes runs in the interpreter, the library's check finds it), and
    // the cheats it runs each frame
    if (!files.mods.empty()) {
        std::filesystem::path mods = path_of(files.mods);
        std::error_code ec;
        if (std::filesystem::is_regular_file(mods / "code.ips", ec)) {
            std::u8string ips = (mods / "code.ips").u8string();
            if (!apply_ips(read_file(std::string(ips.begin(), ips.end())), code, error)) {
                error = "code.ips: " + error;
                return false;
            }
            std::printf("mods: code.ips laid over the program\n");
        }
        if (std::filesystem::is_regular_file(mods / "cheats.txt", ec)) {
            std::u8string file = (mods / "cheats.txt").u8string();
            std::vector<uint8_t> text = read_file(std::string(file.begin(), file.end()));
            cheats = parse_cheats(std::string(text.begin(), text.end()));
            std::printf("mods: %zu cheats\n", cheats.size());
        }
    }
    if (!files.game.empty()) title = read_title(path_of(files.game));
    std::string library_path = files.library;
    if (library_path.empty()) {
        std::u8string installed = installed_library(exheader.program_id).u8string();
        library_path.assign(installed.begin(), installed.end());
    }
    if (!library.open(library_path, error)) {
        error += "\n(recompile the game first: 3dsrecomp build, or the port's launcher)";
        return false;
    }

    config.app_bytes = exheader.app_bytes;
    config.region = exheader.region;
    services = std::make_unique<Services>(exheader.program_id);
    fs = std::make_unique<Fs>(files.user, exheader.program_id);
    if (!files.game.empty()) {
        fs->romfs = find_romfs(path_of(files.game), error);
        if (!fs->romfs) return false;
    }
    if (!files.update.empty()) fs->update_romfs = find_romfs(path_of(files.update), error);
    services->fs = fs.get();
    // the decomp in place of the functions it replaces: recompiled with the
    // library, or in the interpreter where the library does not have it
    if (!files.decomp.empty()) {
        decomp = std::make_unique<Decomp>();
        if (!read_decomp(files.decomp, *decomp, error)) return false;
    }
    open_session = this;
    system = std::make_unique<System>(config, lookup, serve);
    system->set_redirect(redirect);
    services->on_module = [this, on_module](const std::string &name, uint32_t base) {
        module_changed(library, system->memory, decomp.get(), services->cro, name, base);
        if (on_module) on_module(name, base);
    };
    load(*system, exheader, code, decomp.get());
    return link_code(library, system->memory, decomp.get(), error);
}

} // namespace runtime
