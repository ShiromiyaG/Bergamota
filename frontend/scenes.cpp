#include "scenes.h"
#include "settings.h"

#include <toml++/toml.hpp>

#include <algorithm>

namespace frontend {

bool Scenes::read(const std::filesystem::path &file, std::string &error) {
    list.clear();
    std::error_code missing;
    if (!std::filesystem::exists(file, missing)) return true;
    toml::table t;
    try {
        t = toml::parse_file(utf8(file));
    } catch (const toml::parse_error &e) {
        error = utf8(file) + ": " + std::string(e.description());
        return false;
    }
    if (const toml::array *scenes = t["scene"].as_array()) {
        for (const toml::node &node : *scenes) {
            const toml::table *s = node.as_table();
            if (!s) continue;
            Scene scene;
            scene.name = (*s)["name"].value_or(std::string());
            scene.layout = (*s)["layout"].value_or(std::string("panel"));
            scene.lift = float((*s)["lift"].value_or(0.0));
            if (const toml::array *modules = (*s)["modules"].as_array()) {
                for (const toml::node &m : *modules) {
                    if (auto name = m.value<std::string>()) scene.modules.push_back(*name);
                }
            }
            list.push_back(std::move(scene));
        }
    }
    return true;
}

const Scene *Scenes::pick(const std::set<std::string> &loaded) const {
    for (const Scene &s : list) {
        if (std::all_of(s.modules.begin(), s.modules.end(), [&](const std::string &m) { return loaded.count(m) != 0; })) return &s;
    }
    return nullptr;
}

} // namespace frontend
