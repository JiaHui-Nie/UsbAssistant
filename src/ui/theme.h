// 明暗配色与系统主题检测。
#pragma once

#include <windows.h>
#include <d2d1.h>

#include <cstdint>

namespace usb::ui {

D2D1_COLOR_F Rgb(std::uint32_t rgb, float alpha = 1.0f);
COLORREF ToColorRef(const D2D1_COLOR_F& color);
bool IsSystemDarkMode();

struct Palette {
    bool dark = false;

    D2D1_COLOR_F panel;
    D2D1_COLOR_F panel_border;
    D2D1_COLOR_F card;
    D2D1_COLOR_F card_border;
    D2D1_COLOR_F text_primary;
    D2D1_COLOR_F text_secondary;
    D2D1_COLOR_F text_tertiary;

    D2D1_COLOR_F accent;
    D2D1_COLOR_F on_accent;
    D2D1_COLOR_F accent_soft;
    D2D1_COLOR_F secondary_button;
    D2D1_COLOR_F secondary_text;

    D2D1_COLOR_F track;
    D2D1_COLOR_F hover_fill;
    D2D1_COLOR_F icon_tile;

    D2D1_COLOR_F warn;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F success;
    D2D1_COLOR_F toast;
    D2D1_COLOR_F toast_border;
    D2D1_COLOR_F on_toast;
};

Palette MakePalette(bool dark);

}  // namespace usb::ui
