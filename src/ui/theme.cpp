#include "ui/theme.h"

namespace usb::ui {

D2D1_COLOR_F Rgb(std::uint32_t rgb, float alpha) {
    D2D1_COLOR_F color = {};
    color.r = ((rgb >> 16) & 0xFF) / 255.0f;
    color.g = ((rgb >> 8) & 0xFF) / 255.0f;
    color.b = (rgb & 0xFF) / 255.0f;
    color.a = alpha;
    return color;
}

COLORREF ToColorRef(const D2D1_COLOR_F& color) {
    return RGB(static_cast<BYTE>(color.r * 255.0f),
               static_cast<BYTE>(color.g * 255.0f),
               static_cast<BYTE>(color.b * 255.0f));
}

bool IsSystemDarkMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                      L"Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return value == 0;
}

Palette MakePalette(bool dark) {
    Palette palette;
    palette.dark = dark;
    if (!dark) {
        palette.panel = Rgb(0xF6F6F8, 0.985f);
        palette.panel_border = Rgb(0x000000, 0.10f);
        palette.card = Rgb(0xFFFFFF, 1.0f);
        palette.card_border = Rgb(0x000000, 0.055f);
        palette.text_primary = Rgb(0x1D1D1F);
        palette.text_secondary = Rgb(0x6E6E73);
        palette.text_tertiary = Rgb(0x8A8A8E);

        palette.accent = Rgb(0x0A7AFF);
        palette.on_accent = Rgb(0xFFFFFF);
        palette.accent_soft = Rgb(0x0A7AFF, 0.12f);
        palette.secondary_button = Rgb(0xEDEDF0);
        palette.secondary_text = Rgb(0x3C3C43);

        palette.track = Rgb(0x000000, 0.085f);
        palette.hover_fill = Rgb(0x000000, 0.055f);
        palette.icon_tile = Rgb(0x0A7AFF, 0.10f);

        palette.warn = Rgb(0xFF9500);
        palette.danger = Rgb(0xFF3B30);
        palette.success = Rgb(0x30D158);
        palette.toast = Rgb(0xFFFFFF, 0.97f);
        palette.toast_border = Rgb(0x000000, 0.12f);
        palette.on_toast = Rgb(0x1D1D1F);
    } else {
        palette.panel = Rgb(0x1C1C1E, 0.985f);
        palette.panel_border = Rgb(0xFFFFFF, 0.12f);
        palette.card = Rgb(0x2A2A2D);
        palette.card_border = Rgb(0xFFFFFF, 0.065f);
        palette.text_primary = Rgb(0xF2F2F7);
        palette.text_secondary = Rgb(0xA0A0A8);
        palette.text_tertiary = Rgb(0x8A8A90);

        palette.accent = Rgb(0x0A84FF);
        palette.on_accent = Rgb(0xFFFFFF);
        palette.accent_soft = Rgb(0x0A84FF, 0.16f);
        palette.secondary_button = Rgb(0x3A3A3E);
        palette.secondary_text = Rgb(0xE6E6EB);

        palette.track = Rgb(0xFFFFFF, 0.10f);
        palette.hover_fill = Rgb(0xFFFFFF, 0.09f);
        palette.icon_tile = Rgb(0x0A84FF, 0.16f);

        palette.warn = Rgb(0xFF9F0A);
        palette.danger = Rgb(0xFF453A);
        palette.success = Rgb(0x32D74B);
        palette.toast = Rgb(0x2C2C2E, 0.94f);
        palette.toast_border = Rgb(0xFFFFFF, 0.14f);
        palette.on_toast = Rgb(0xF2F2F7);
    }
    return palette;
}

}  // namespace usb::ui
