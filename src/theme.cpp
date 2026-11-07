#include "theme.h"

D2D1_COLOR_F Rgb(unsigned int rgb, float alpha) {
    D2D1_COLOR_F c = {};
    c.r = ((rgb >> 16) & 0xFF) / 255.0f;
    c.g = ((rgb >> 8) & 0xFF) / 255.0f;
    c.b = (rgb & 0xFF) / 255.0f;
    c.a = alpha;
    return c;
}

bool QuerySystemDarkMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
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
    Palette p;
    p.dark = dark;
    if (!dark) {
        p.panel        = Rgb(0xF6F6F8, 0.985f);
        p.panelBorder  = Rgb(0x000000, 0.10f);
        p.card         = Rgb(0xFFFFFF, 1.0f);
        p.cardBorder   = Rgb(0x000000, 0.055f);
        p.textPrimary  = Rgb(0x1D1D1F);
        p.textSecondary= Rgb(0x6E6E73);
        p.textTertiary = Rgb(0x8A8A8E);

        p.accent       = Rgb(0x0A7AFF);
        p.onAccent     = Rgb(0xFFFFFF);
        p.accentSoft   = Rgb(0x0A7AFF, 0.12f);
        p.secondaryBtn = Rgb(0xEDEDF0);
        p.secondaryText= Rgb(0x3C3C43);

        p.track        = Rgb(0x000000, 0.085f);
        p.hoverFill    = Rgb(0x000000, 0.055f);
        p.iconTile     = Rgb(0x0A7AFF, 0.10f);

        p.warn         = Rgb(0xFF9500);
        p.danger       = Rgb(0xFF3B30);
        p.success      = Rgb(0x30D158);
        p.toast        = Rgb(0x1C1C1E, 0.94f);
        p.onToast      = Rgb(0xFFFFFF);
    } else {
        p.panel        = Rgb(0x1C1C1E, 0.985f);
        p.panelBorder  = Rgb(0xFFFFFF, 0.12f);
        p.card         = Rgb(0x2A2A2D);
        p.cardBorder   = Rgb(0xFFFFFF, 0.065f);
        p.textPrimary  = Rgb(0xF2F2F7);
        p.textSecondary= Rgb(0xA0A0A8);
        p.textTertiary = Rgb(0x8A8A90);

        p.accent       = Rgb(0x0A84FF);
        p.onAccent     = Rgb(0xFFFFFF);
        p.accentSoft   = Rgb(0x0A84FF, 0.16f);
        p.secondaryBtn = Rgb(0x3A3A3E);
        p.secondaryText= Rgb(0xE6E6EB);

        p.track        = Rgb(0xFFFFFF, 0.10f);
        p.hoverFill    = Rgb(0xFFFFFF, 0.09f);
        p.iconTile     = Rgb(0x0A84FF, 0.16f);

        p.warn         = Rgb(0xFF9F0A);
        p.danger       = Rgb(0xFF453A);
        p.success      = Rgb(0x32D74B);
        p.toast        = Rgb(0x2C2C2E, 0.94f);
        p.onToast      = Rgb(0xF2F2F7);
    }
    return p;
}
