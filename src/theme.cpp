#include "theme.h"

#include "imgui_internal.h"  // ImTextCharFromUtf8

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace bones {

ImVec4 Hex(unsigned rgb, float a) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

ImU32 U32(const ImVec4& c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

namespace {

// Order: bg, surface, surface2, border, text, muted, accent, accentLo,
// chip, bad, star, viewerBg.
const Palette kPalette = {
    Hex(0x0D0D12), Hex(0x15151E), Hex(0x1E1E2A), Hex(0x28283A), Hex(0xD4D4E8), Hex(0x7A7A96),
    Hex(0x7C6AFF), Hex(0x7C6AFF, 0.16f), Hex(0x271E5E), Hex(0xE07070), Hex(0xF5C542), Hex(0x09090C),
};

std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// fontData reads a font file once, so a font merged into several faces
// shares one copy in memory.
std::vector<char>* fontData(const std::string& path) {
    static std::map<std::string, std::vector<char>> files;
    auto it = files.find(path);
    if (it != files.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<char>& data = files[path];
    std::ifstream f(path, std::ios::binary);
    if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return data.empty() ? nullptr : &data;
}

ImFont* addFont(const std::string& path, ImFontConfig cfg, const ImWchar* ranges = nullptr) {
    std::vector<char>* data = fontData(path);
    if (!data) return nullptr;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data->data(), int(data->size()), kBody, &cfg, ranges);
}

}  // namespace

const Palette& Colours() { return kPalette; }

void ApplyTheme(float scale) {
    const Palette& p = kPalette;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(9, 6);
    s.ItemSpacing = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 10;
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 5;
    s.GrabRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.FontSizeBase = kBody;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;

    ImVec4* c = s.Colors;
    ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = p.bg;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = p.surface2;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = p.surface2;
    c[ImGuiCol_FrameBgHovered] = p.border;
    c[ImGuiCol_FrameBgActive] = p.border;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = p.border;
    c[ImGuiCol_ScrollbarGrabHovered] = p.muted;
    c[ImGuiCol_ScrollbarGrabActive] = p.muted;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_Button] = p.surface2;
    c[ImGuiCol_ButtonHovered] = p.border;
    c[ImGuiCol_ButtonActive] = p.chip;
    c[ImGuiCol_Header] = p.accentLo;
    c[ImGuiCol_HeaderHovered] = p.surface2;
    c[ImGuiCol_HeaderActive] = p.chip;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_Tab] = none;
    c[ImGuiCol_TabHovered] = p.surface2;
    c[ImGuiCol_TabSelected] = p.surface2;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TabDimmed] = none;
    c[ImGuiCol_TabDimmedSelected] = p.surface2;
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_NavCursor] = none;  // no keyboard focus outline
}

Fonts LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    std::string symbols = fontPath("seguisym.ttf");
    auto add = [&](const char* file) -> ImFont* {
        // Segoe UI's line box has more room above the letters than
        // below, so text drawn in a box sits low; lift it 2 px (at 15 px).
        ImFontConfig text;
        text.GlyphOffset = ImVec2(0, -2);
        ImFont* f = addFont(fontPath(file), text);
        if (!f) return io.Fonts->AddFontDefault();
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.GlyphOffset = ImVec2(0, 1);
        static const ImWchar range[] = {0xE700, 0xF8FF, 0};
        addFont(iconFont, merge, range);
        ImFontConfig sym;
        sym.MergeMode = true;
        addFont(symbols, sym);
        return f;
    };
    Fonts f;
    f.regular = add("segoeui.ttf");
    f.semibold = add("seguisb.ttf");
    io.FontDefault = f.regular;
    return f;
}

ImVec4 InkBounds(const char* s) {
    ImFontBaked* baked = ImGui::GetFontBaked();
    float x = 0;
    ImVec4 r(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const char* p = s; *p;) {
        unsigned int c = 0;
        p += ImTextCharFromUtf8(&c, p, nullptr);
        const ImFontGlyph* g = baked->FindGlyph(ImWchar(c));
        if (!g) continue;
        if (g->Visible) {
            r.x = std::min(r.x, x + g->X0);
            r.y = std::min(r.y, g->Y0);
            r.z = std::max(r.z, x + g->X1);
            r.w = std::max(r.w, g->Y1);
        }
        x += g->AdvanceX;
    }
    if (r.x > r.z) r = ImVec4(0, 0, x, ImGui::GetFontSize());
    return r;
}

void DrawCentred(ImDrawList* dl, ImVec2 centre, const char* s, ImU32 col) {
    ImVec4 b = InkBounds(s);
    dl->AddText(ImVec2(std::round(centre.x - (b.x + b.z) / 2), std::round(centre.y - (b.y + b.w) / 2)), col, s);
}

float TextTop(float centreY) {
    // Centre the capitals: from the top of an H to the baseline.
    const ImFontGlyph* g = ImGui::GetFontBaked()->FindGlyph('H');
    float mid = g ? (g->Y0 + g->Y1) / 2 : ImGui::GetFontSize() / 2;
    return std::round(centreY - mid);
}

// Ellipsis cuts text to fit a width, ending in "…".
std::string Ellipsis(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    const char* dots = "\xE2\x80\xA6";
    float dw = ImGui::CalcTextSize(dots).x;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && (s[mid] & 0xC0) == 0x80) mid--;  // whole characters
        if (mid <= lo) break;
        if (ImGui::CalcTextSize(s.c_str(), s.c_str() + mid).x + dw <= width) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && (s[lo] & 0xC0) == 0x80) lo--;
    return s.substr(0, lo) + dots;
}

}  // namespace bones
