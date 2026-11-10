// How the launcher looks: the port's colors (each game's, from port.toml), a
// background painted from them (shapes of our own, no picture), Nunito, and
// the pieces the pages share.
#ifndef LAUNCHER_LOOK_H
#define LAUNCHER_LOOK_H

#include <imgui.h>

#include <cstdint>

#include <functional>
#include <optional>
#include <string>

namespace launcher::look {

constexpr ImU32 rgb(uint32_t hex, uint8_t alpha = 255) {
    return IM_COL32(hex >> 16 & 0xFF, hex >> 8 & 0xFF, hex & 0xFF, alpha);
}

// the ground, the pages' panel over it, and the words
constexpr ImU32 INK = rgb(0x101116), PANEL = rgb(0x16171E), LINE = rgb(0x2A2C36), TEXT = rgb(0xECECF1), WEAK = rgb(0x9A9CA8),
                GOOD = rgb(0x6FCF8E), WARN = rgb(0xE8B04A), BAD = rgb(0xE85A5A), WIDGET = rgb(0x22242D), HOVER = rgb(0x2C2E39);

// "#RRGGBB"
std::optional<ImU32> hex(const std::string &text);
// a game's color, by its title id, and the port's next game's
ImU32 accent(const std::string &title);
ImU32 partner(const std::string &title);
ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 alpha(ImU32 color, float a);

// the fonts, at the sizes the pages use
struct Fonts {
    ImFont *body, *small, *chip, *large, *game;
    ImFont *title15, *title17, *title22, *title26, *title32, *title56, *title76;
};
const Fonts &fonts();

// Nunito and a dark style in the port's first color, at the display's scale
void install(float scale);
float scale();

// the window's ground: dark above, the game's color rising from below with
// the port's other game across, rings around the glow
void background(ImDrawList *draw, ImVec2 min, ImVec2 max, ImU32 accent, ImU32 partner);

// a small note in a rounded outline, its width
float chip_width(const char *text);
void chip(ImVec2 at, const char *text, ImU32 color);

// the big button (Play, Set up) at a place
bool big_button(const char *id, const char *text, ImVec2 at, ImU32 accent);
ImVec2 big_button_size();

// a page's title, a section in the game's color, a setting with its control
void page_title(const char *title, const char *about);
void section(const char *text, ImU32 accent);
void row(const char *name, const char *about, float control_width, const std::function<void()> &control);

} // namespace launcher::look

#endif
