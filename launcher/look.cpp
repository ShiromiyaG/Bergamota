#include "look.h"

#include "port.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace launcher::look {

namespace {
#include "nunito_semibold.inc"
#include "nunito_black.inc"

Fonts loaded;
float ui_scale = 1;
constexpr ImU32 DEFAULT_ACCENT = rgb(0xE05A3A);
} // namespace

std::optional<ImU32> hex(const std::string &text) {
    std::string digits = text;
    if (!digits.empty() && digits[0] == '#') digits.erase(0, 1);
    if (digits.size() != 6 || digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return std::nullopt;
    return rgb(uint32_t(std::stoul(digits, nullptr, 16)));
}

ImU32 accent(const std::string &title) {
    const Port *p = get_port();
    const GameInfo *g = p ? p->game(title) : nullptr;
    return g ? hex(g->color).value_or(DEFAULT_ACCENT) : DEFAULT_ACCENT;
}

ImU32 partner(const std::string &title) {
    const Port *p = get_port();
    if (!p || p->games.empty()) return DEFAULT_ACCENT;
    size_t at = 0;
    for (size_t i = 0; i < p->games.size(); i++)
        if (upper(p->games[i].title) == upper(title)) at = i;
    return accent(p->games[(at + 1) % p->games.size()].title);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    auto lerp = [&](int shift) {
        float x = float(a >> shift & 0xFF), y = float(b >> shift & 0xFF);
        return uint32_t(std::lround(x + (y - x) * t)) << shift;
    };
    return lerp(IM_COL32_R_SHIFT) | lerp(IM_COL32_G_SHIFT) | lerp(IM_COL32_B_SHIFT) | (a & IM_COL32_A_MASK);
}

ImU32 alpha(ImU32 color, float a) {
    uint32_t value = uint32_t(std::clamp(a, 0.0f, 1.0f) * float(color >> IM_COL32_A_SHIFT & 0xFF));
    return (color & ~IM_COL32_A_MASK) | value << IM_COL32_A_SHIFT;
}

const Fonts &fonts() { return loaded; }
float scale() { return ui_scale; }

void install(float scale) {
    ui_scale = scale > 0 ? scale : 1;
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    auto font = [&](const unsigned char *data, size_t size, float px) {
        // Latin-1, and the dashes, quotes, bullet and ellipsis
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2039, 0x203A, 0};
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(data), int(size), px * ui_scale, &config, ranges);
    };
    // the first is ImGui's default: the body
    loaded.body = font(NUNITO_SEMIBOLD, sizeof NUNITO_SEMIBOLD, 16.0f);
    loaded.small = font(NUNITO_SEMIBOLD, sizeof NUNITO_SEMIBOLD, 13.5f);
    loaded.chip = font(NUNITO_SEMIBOLD, sizeof NUNITO_SEMIBOLD, 13.0f);
    loaded.large = font(NUNITO_SEMIBOLD, sizeof NUNITO_SEMIBOLD, 18.0f);
    loaded.game = font(NUNITO_SEMIBOLD, sizeof NUNITO_SEMIBOLD, 16.0f);
    loaded.title15 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 15.0f);
    loaded.title17 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 17.0f);
    loaded.title22 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 22.0f);
    loaded.title26 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 26.0f);
    loaded.title32 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 32.0f);
    loaded.title56 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 56.0f);
    loaded.title76 = font(NUNITO_BLACK, sizeof NUNITO_BLACK, 76.0f);

    const Port *p = get_port();
    const ImU32 first = p && !p->games.empty() ? accent(p->games.front().title) : DEFAULT_ACCENT;
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(12, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.FrameRounding = 6;
    style.GrabRounding = 6;
    style.PopupRounding = 6;
    style.ScrollbarRounding = 6;
    style.ChildRounding = 0;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.FrameBorderSize = 1;
    style.PopupBorderSize = 1;
    style.ScrollbarSize = 10;
    style.GrabMinSize = 14;
    auto c = [&](ImGuiCol col, ImU32 value) { style.Colors[col] = ImGui::ColorConvertU32ToFloat4(value); };
    c(ImGuiCol_Text, TEXT);
    c(ImGuiCol_TextDisabled, WEAK);
    c(ImGuiCol_WindowBg, alpha(INK, 0));
    c(ImGuiCol_ChildBg, alpha(INK, 0));
    c(ImGuiCol_PopupBg, PANEL);
    c(ImGuiCol_Border, LINE);
    c(ImGuiCol_FrameBg, WIDGET);
    c(ImGuiCol_FrameBgHovered, HOVER);
    c(ImGuiCol_FrameBgActive, mix(HOVER, first, 0.35f));
    c(ImGuiCol_Button, WIDGET);
    c(ImGuiCol_ButtonHovered, HOVER);
    c(ImGuiCol_ButtonActive, mix(HOVER, first, 0.4f));
    c(ImGuiCol_Header, mix(WIDGET, first, 0.5f));
    c(ImGuiCol_HeaderHovered, HOVER);
    c(ImGuiCol_HeaderActive, mix(HOVER, first, 0.5f));
    c(ImGuiCol_CheckMark, TEXT);
    c(ImGuiCol_SliderGrab, first);
    c(ImGuiCol_SliderGrabActive, mix(first, TEXT, 0.25f));
    c(ImGuiCol_ScrollbarBg, alpha(INK, 0));
    c(ImGuiCol_ScrollbarGrab, LINE);
    c(ImGuiCol_ScrollbarGrabHovered, HOVER);
    c(ImGuiCol_ScrollbarGrabActive, first);
    c(ImGuiCol_PlotHistogram, first);
    c(ImGuiCol_NavHighlight, first);
    c(ImGuiCol_Separator, LINE);
    style.ScaleAllSizes(ui_scale);
}

namespace {
// a soft disc of color fading out to nothing
void glow(ImDrawList *draw, ImVec2 center, float radius, ImU32 color) {
    constexpr int STEPS = 72;
    const ImVec2 uv = draw->_Data->TexUvWhitePixel;
    draw->PrimReserve(STEPS * 3, STEPS + 1);
    const ImDrawIdx base = ImDrawIdx(draw->_VtxCurrentIdx);
    draw->PrimWriteVtx(center, uv, color);
    for (int i = 0; i < STEPS; i++) {
        float angle = float(i) / STEPS * 6.2831853f;
        draw->PrimWriteVtx(ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius), uv, alpha(color, 0));
    }
    for (int i = 0; i < STEPS; i++) {
        draw->PrimWriteIdx(base);
        draw->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        draw->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % STEPS));
    }
}
} // namespace

void background(ImDrawList *draw, ImVec2 min, ImVec2 max, ImU32 accent, ImU32 partner) {
    const float w = max.x - min.x, h = max.y - min.y, size = std::max(w, h);
    draw->AddRectFilled(min, max, INK);
    draw->AddRectFilledMultiColor(min, max, INK, INK, mix(INK, accent, 0.42f), mix(INK, partner, 0.22f));
    const ImVec2 sun(max.x - w * 0.16f, max.y - h * 0.08f);
    glow(draw, sun, size * 0.62f, alpha(accent, 0.38f));
    glow(draw, ImVec2(min.x + w * 0.08f, min.y + h * 0.18f), size * 0.42f, alpha(partner, 0.16f));
    for (int ring = 1; ring <= 5; ring++)
        draw->AddCircle(sun, size * (0.12f + ring * 0.085f), alpha(accent, 0.11f - ring * 0.015f), 128, ui_scale);
}

float chip_width(const char *text) {
    return loaded.chip->CalcTextSizeA(loaded.chip->FontSize, FLT_MAX, 0, text).x + 28 * ui_scale;
}

void chip(ImVec2 at, const char *text, ImU32 color) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 size(chip_width(text), loaded.chip->FontSize + 6 * ui_scale);
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), alpha(INK, 0.55f), 10 * ui_scale);
    draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), alpha(color, 0.8f), 10 * ui_scale, 0, ui_scale);
    // a dot of its color, then what it says
    draw->AddCircleFilled(ImVec2(at.x + 12 * ui_scale, at.y + size.y / 2), 3 * ui_scale, color);
    draw->AddText(loaded.chip, loaded.chip->FontSize, ImVec2(at.x + 19 * ui_scale, at.y + 3 * ui_scale), color, text);
}

ImVec2 big_button_size() { return ImVec2(210 * ui_scale, 60 * ui_scale); }

bool big_button(const char *id, const char *text, ImVec2 at, ImU32 accent) {
    const ImVec2 size = big_button_size();
    ImGui::SetCursorScreenPos(at);
    bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImU32 fill = held ? mix(accent, INK, 0.15f) : hovered ? mix(accent, TEXT, 0.1f) : accent;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), fill, 8 * ui_scale);
    draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), mix(accent, rgb(0xFFFFFF), 0.25f), 8 * ui_scale, 0, ui_scale);
    ImFont *font = loaded.title26;
    ImVec2 text_size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text);
    draw->AddText(font, font->FontSize, ImVec2(at.x + (size.x - text_size.x) / 2, at.y + (size.y - text_size.y) / 2), rgb(0xFFFFFF), text);
    if (ImGui::IsItemFocused()) draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), TEXT, 8 * ui_scale, 0, 2 * ui_scale);
    return clicked;
}

void page_title(const char *title, const char *about) {
    ImGui::Dummy(ImVec2(0, 18 * ui_scale));
    ImGui::PushFont(loaded.title32);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (about && *about) {
        ImGui::PushStyleColor(ImGuiCol_Text, WEAK);
        ImGui::TextWrapped("%s", about);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 4 * ui_scale));
}

void section(const char *text, ImU32 accent) {
    ImGui::Dummy(ImVec2(0, 10 * ui_scale));
    std::string upper_text = upper(text);
    ImGui::PushFont(loaded.title15);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextUnformatted(upper_text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y), LINE, ui_scale);
    ImGui::Dummy(ImVec2(0, 6 * ui_scale));
}

void row(const char *name, const char *about, float control_width, const std::function<void()> &control) {
    ImGui::PushID(name);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::PushTextWrapPos(start.x + width * 0.58f);
    ImGui::TextUnformatted(name);
    if (about && *about) {
        ImGui::PushFont(loaded.small);
        ImGui::PushStyleColor(ImGuiCol_Text, WEAK);
        ImGui::TextWrapped("%s", about);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    const float end = ImGui::GetCursorPosY();
    const float frame = ImGui::GetFrameHeight();
    const float block = end - start.y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::SetCursorPos(ImVec2(start.x + width - control_width, start.y + std::max(0.0f, (block - frame) / 2)));
    ImGui::SetNextItemWidth(control_width);
    control();
    ImGui::SetCursorPosY(std::max(end, ImGui::GetCursorPosY()) + 4 * ui_scale);
    ImGui::PopID();
}

} // namespace launcher::look
