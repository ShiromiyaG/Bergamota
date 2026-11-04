// decomp.txt's option lines.

#include "runtime/decomp_options.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace runtime {

bool parse_option(const std::string &line, DecompOption &out) {
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string word; in >> word;) words.push_back(word);
    if (words.size() < 4 || words[0] != "option") return false;
    DecompOption option;
    option.symbol = words[1];
    option.name = option.symbol.rfind("port_", 0) == 0 ? option.symbol.substr(5) : option.symbol;
    if (words[2] == "int") {
        if (words.size() < 6) return false;
        option.toggle = false;
        option.min = int32_t(std::strtol(words[4].c_str(), nullptr, 10));
        option.max = int32_t(std::strtol(words[5].c_str(), nullptr, 10));
    } else if (words[2] != "bool") {
        return false;
    }
    option.value = int32_t(std::strtol(words[3].c_str(), nullptr, 10));
    // the text as written, spaces and all, from its first word on
    size_t first_text = option.toggle ? 4 : 6, at = 0;
    if (words.size() > first_text) {
        for (size_t i = 0; i < first_text; i++) {
            at = line.find_first_not_of(" \t", at);
            at = line.find_first_of(" \t", at);
        }
        std::string text = line.substr(line.find_first_not_of(" \t", at));
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
        size_t bar = text.find('|');
        option.label = text.substr(0, bar);
        if (bar != std::string::npos) option.help = text.substr(bar + 1);
    }
    if (option.label.empty()) option.label = option.name;
    out = option;
    return true;
}

std::vector<DecompOption> read_options(const std::string &dir) {
    std::vector<DecompOption> out;
    std::ifstream file(std::filesystem::path(std::u8string(dir.begin(), dir.end())) / "decomp.txt");
    for (std::string line; std::getline(file, line);) {
        DecompOption option;
        if (parse_option(line, option)) out.push_back(option);
    }
    return out;
}

} // namespace runtime
