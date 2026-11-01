// The single screen's scenes: which layout each part of a game is shown in,
// from a port's file (Solstice's single-screen.toml), by the modules (CROs)
// the game has loaded. A layout:
//   "panel"   the top screen fills the window, the bottom one a panel on a
//             button (Tab, the right stick's click)
//   "stacked" both screens one over the other, as the two-screen view has
//             them, the window's sides filled with the top one blurred:
//             cutscenes and movies made for both
//   "top"     the top screen alone
//
//   [[scene]]
//   name = "movie"
//   modules = ["MovieLib"]   # all loaded; none: any time
//   layout = "stacked"
//
// The first scene whose modules are all loaded is the one; with none, "panel".
#ifndef FRONTEND_SCENES_H
#define FRONTEND_SCENES_H

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace frontend {

struct Scene {
    std::string name, layout;
    std::vector<std::string> modules;
};

struct Scenes {
    std::vector<Scene> list;
    // false, with why, when the file is there and cannot be read
    bool read(const std::filesystem::path &file, std::string &error);
    // the scene for the modules loaded now, null for none
    const Scene *pick(const std::set<std::string> &loaded) const;
};

} // namespace frontend

#endif
