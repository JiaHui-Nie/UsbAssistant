#pragma once

#include <windows.h>
#include <d2d1.h>

// 便捷生成颜色：rgb 为 0xRRGGBB，alpha 为 0..1。
D2D1_COLOR_F Rgb(unsigned int rgb, float alpha = 1.0f);

// 读取系统"应用模式"亮/暗设置。
bool QuerySystemDarkMode();

// macOS Big Sur 风格的两套调色板。
struct Palette {
    bool dark = false;

    D2D1_COLOR_F panel;
    D2D1_COLOR_F panelBorder;
    D2D1_COLOR_F card;
    D2D1_COLOR_F cardBorder;
    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F textTertiary;

    D2D1_COLOR_F accent;
    D2D1_COLOR_F onAccent;
    D2D1_COLOR_F accentSoft;
    D2D1_COLOR_F secondaryBtn;
    D2D1_COLOR_F secondaryText;

    D2D1_COLOR_F track;
    D2D1_COLOR_F hoverFill;
    D2D1_COLOR_F iconTile;

    D2D1_COLOR_F warn;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F success;
    D2D1_COLOR_F toast;
    D2D1_COLOR_F onToast;
};

Palette MakePalette(bool dark);
