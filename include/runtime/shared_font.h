#pragma once

// the console's shared font (APT's GetSharedFont): a block of a status word
// and a BCFNT whose pointers are absolute

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace runtime {

// the block for being mapped at base: the user's dump of the console's
// (user/sysdata/shared_font.bin), or else a stand-in drawn from a host font
// for Japanese, or else printable ASCII from a bitmap; source says which
std::vector<uint8_t> shared_font(const std::filesystem::path &user, uint32_t base, std::string &source);

// the stand-in alone, from a font file's bytes (none: the bitmap one), laid
// out for base; empty when the font has no Japanese
std::vector<uint8_t> shared_font_stand_in(const std::vector<uint8_t> &font, uint32_t base);

// a block's pointers moved from where it was laid out for to base; false
// when it is no font block
bool shared_font_relocate(std::vector<uint8_t> &block, uint32_t base);

} // namespace runtime
