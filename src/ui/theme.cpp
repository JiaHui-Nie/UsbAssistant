#include "ui/theme.h"

#include <cmath>

namespace usb::ui {
namespace {

constexpr std::uint32_t kFallbackAccentLight = 0x0A7AFF;
constexpr std::uint32_t kFallbackAccentDark = 0x0A84FF;

// 读取形如 0xAABBGGRR 的注册表颜色；缺失或类型不符返回 0。
std::uint32_t ReadArgbValue(HKEY root, const wchar_t* sub_key,
                            const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub_key, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, name, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value),
                                         &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD ||
        size != sizeof(DWORD)) {
        return 0;
    }
    return value;
}

float Luminance(std::uint32_t rgb) {
    const float red = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    const float green = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    const float blue = static_cast<float>(rgb & 0xFF) / 255.0f;
    return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
}

std::uint32_t MixRgb(std::uint32_t from, std::uint32_t to, float amount) {
    const auto mix = [amount](int a, int b) {
        return static_cast<int>(std::lround(a + (b - a) * amount));
    };
    const int red = mix(static_cast<int>((from >> 16) & 0xFF),
                        static_cast<int>((to >> 16) & 0xFF));
    const int green = mix(static_cast<int>((from >> 8) & 0xFF),
                          static_cast<int>((to >> 8) & 0xFF));
    const int blue = mix(static_cast<int>(from & 0xFF),
                         static_cast<int>(to & 0xFF));
    return (static_cast<std::uint32_t>(red) << 16) |
           (static_cast<std::uint32_t>(green) << 8) |
           static_cast<std::uint32_t>(blue);
}

// 保证强调色在当前背景上足够显眼：深色主题提亮过暗的颜色，
// 浅色主题压暗过亮的颜色，避免选中态/按钮与背景混在一起。
std::uint32_t EnsureAccentContrast(std::uint32_t accent, bool dark) {
    const std::uint32_t target = dark ? 0xFFFFFF : 0x000000;
    const float limit = dark ? 0.30f : 0.68f;
    float amount = 0.0f;
    float luminance = Luminance(accent);
    while ((dark ? luminance < limit : luminance > limit) && amount < 0.8f) {
        amount += 0.05f;
        luminance = Luminance(MixRgb(accent, target, amount));
    }
    return MixRgb(accent, target, amount);
}

}  // namespace

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

bool SystemAccentColor(std::uint32_t* rgb) {
    if (!rgb) return false;
    // DWM 与 Explorer 都把强调色存成 0xAABBGGRR（ABGR，低字节是红）；
    // Alpha 为 0 视为未设置。
    std::uint32_t value = ReadArgbValue(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
        L"AccentColor");
    if (((value >> 24) & 0xFF) == 0) {
        value = ReadArgbValue(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
            L"AccentColorMenu");
    }
    if (((value >> 24) & 0xFF) == 0) return false;
    const std::uint32_t red = value & 0xFF;
    const std::uint32_t green = (value >> 8) & 0xFF;
    const std::uint32_t blue = (value >> 16) & 0xFF;
    *rgb = (red << 16) | (green << 8) | blue;
    return true;
}

Palette MakePalette(bool dark) {
    Palette palette;
    palette.dark = dark;

    // 强调色优先跟随系统；读不到时退回原来的蓝色系。
    std::uint32_t accent = dark ? kFallbackAccentDark : kFallbackAccentLight;
    std::uint32_t system_accent = 0;
    if (SystemAccentColor(&system_accent)) accent = system_accent;
    accent = EnsureAccentContrast(accent, dark);
    const std::uint32_t on_accent =
        Luminance(accent) < 0.6f ? 0xFFFFFF : 0x1D1D1F;

    if (!dark) {
        palette.panel = Rgb(0xF6F6F8, 0.985f);
        palette.panel_border = Rgb(0x000000, 0.10f);
        palette.card = Rgb(0xFFFFFF, 1.0f);
        palette.card_border = Rgb(0x000000, 0.055f);
        palette.text_primary = Rgb(0x1D1D1F);
        palette.text_secondary = Rgb(0x6E6E73);
        palette.text_tertiary = Rgb(0x8A8A8E);

        palette.accent = Rgb(accent);
        palette.on_accent = Rgb(on_accent);
        palette.accent_soft = Rgb(accent, 0.12f);
        palette.secondary_button = Rgb(0xEDEDF0);
        palette.secondary_text = Rgb(0x3C3C43);

        palette.track = Rgb(0x000000, 0.085f);
        palette.hover_fill = Rgb(0x000000, 0.055f);
        palette.icon_tile = Rgb(accent, 0.10f);

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

        palette.accent = Rgb(accent);
        palette.on_accent = Rgb(on_accent);
        palette.accent_soft = Rgb(accent, 0.16f);
        palette.secondary_button = Rgb(0x3A3A3E);
        palette.secondary_text = Rgb(0xE6E6EB);

        palette.track = Rgb(0xFFFFFF, 0.10f);
        palette.hover_fill = Rgb(0xFFFFFF, 0.09f);
        palette.icon_tile = Rgb(accent, 0.16f);

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
