// A game opened on the runtime the way the player and boot both open it:
// the program, the recompiled library, the services with the file system
// and the romfs, the decomp, the system loaded and linked
// (docs/organization.md, 3). The GPU, the window, input and sound stay the
// caller's. One a process: System takes plain functions, which reach the
// session open.
#ifndef RUNTIME_SESSION_H
#define RUNTIME_SESSION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "runtime/decomp.h"
#include "runtime/fs.h"
#include "runtime/loader.h"
#include "runtime/services.h"
#include "runtime/system.h"

namespace runtime {

class Session {
public:
    struct Files {
        // the program's exheader and code, or both empty for the ones in
        // the dump (the update's when there is one)
        std::string exheader, code;
        // the library, empty for where 3dsrecomp build installs it
        std::string library;
        // the game's dump and its update (empty for none: no romfs then)
        std::string game, update;
        // where saves and extra data go
        std::string user = "user";
        // a folder with the decomp's decomp.bin and decomp.txt, or empty
        std::string decomp;
    };
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // opens files into config's system; false with error when it cannot.
    // config's app bytes and region come from the exheader. on_module, when
    // set, hears each module loaded or gone after its code is linked
    bool open(const Files &files, Config config, std::string &error,
              std::function<void(const std::string &, uint32_t)> on_module = {});

    ExHeader exheader;
    std::string title; // as the game's icon names it, or empty
    Library library;
    std::unique_ptr<Services> services;
    std::unique_ptr<Fs> fs;
    std::unique_ptr<Decomp> decomp;
    std::unique_ptr<System> system;
};

} // namespace runtime

#endif
