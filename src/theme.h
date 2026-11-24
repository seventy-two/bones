// Colours, fonts and ImGui style: kiln's dark purple look.
#pragma once

#include "imgui.h"

namespace shelf {

struct Palette {
    ImVec4 bg, surface, surface2, border, text, muted, accent, accentLo, chip, bad, star, viewerBg;
};

struct Fonts {
    ImFont* regular = nullptr;  // with the icons merged in
    ImFont* semibold = nullptr;  // also with icons
};

// Base sizes, before DPI scaling.
constexpr float kBody = 15, kSmall = 13, kTitle = 17;

namespace icon {
inline constexpr const char* Back = "\xEE\x9C\xAB";  // U+E72B
inline constexpr const char* Forward = "\xEE\x9C\xAA";  // U+E72A
inline constexpr const char* Up = "\xEE\x9D\x8A";  // U+E74A
inline constexpr const char* Refresh = "\xEE\x9C\xAC";  // U+E72C
inline constexpr const char* Search = "\xEE\x9C\xA1";  // U+E721
inline constexpr const char* Grid = "\xEF\x83\xA2";  // U+F0E2
inline constexpr const char* List = "\xEE\xA3\xBD";  // U+E8FD
inline constexpr const char* Pin = "\xEE\x9C\x98";  // U+E718
inline constexpr const char* Unpin = "\xEE\x9D\xBA";  // U+E77A
inline constexpr const char* Folder = "\xEE\xA2\xB7";  // U+E8B7
inline constexpr const char* Drive = "\xEE\xB6\xA2";  // U+EDA2
inline constexpr const char* Usb = "\xEE\xA2\x8E";  // U+E88E
inline constexpr const char* Disc = "\xEE\xA5\x98";  // U+E958
inline constexpr const char* Network = "\xEE\xA3\x8E";  // U+E8CE
inline constexpr const char* Linux = "\xEE\x9D\x96";  // U+E756
inline constexpr const char* Home = "\xEE\xA0\x8F";  // U+E80F
inline constexpr const char* Add = "\xEE\x9C\x90";  // U+E710
inline constexpr const char* Copy = "\xEE\xA3\x88";  // U+E8C8
inline constexpr const char* Cut = "\xEE\xA3\x86";  // U+E8C6
inline constexpr const char* Paste = "\xEE\x9D\xBF";  // U+E77F
inline constexpr const char* Delete = "\xEE\x9D\x8D";  // U+E74D
inline constexpr const char* Rename = "\xEE\xA2\xAC";  // U+E8AC
inline constexpr const char* Open = "\xEE\xA3\xA5";  // U+E8E5
inline constexpr const char* OpenWith = "\xEE\x9E\xAC";  // U+E7AC
inline constexpr const char* Info = "\xEE\xA5\x86";  // U+E946
inline constexpr const char* More = "\xEE\x9C\x92";  // U+E712
inline constexpr const char* NewFolder = "\xEE\xA3\xB4";  // U+E8F4
inline constexpr const char* Sort = "\xEE\xA3\x8B";  // U+E8CB
inline constexpr const char* Hidden = "\xEE\x9E\xB3";  // U+E7B3
inline constexpr const char* Down = "\xEE\x9C\x8D";  // U+E70D
inline constexpr const char* UpArrow = "\xEE\x9C\x8E";  // U+E70E
inline constexpr const char* Dismiss = "\xEE\x9C\x91";  // U+E711
inline constexpr const char* Link = "\xEE\x9C\x9B";  // U+E71B
}  // namespace icon

void ApplyTheme(float scale);
const Palette& Colours();
// LoadFonts adds the fonts to ImGui's atlas, once at startup; with 1.92's
// dynamic atlas they're drawn at any size.
Fonts LoadFonts();

ImU32 U32(const ImVec4& c, float alpha = 1.0f);
ImVec4 Hex(unsigned rgb, float a = 1.0f);

}  // namespace shelf
