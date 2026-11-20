// D2D 基础图形与矢量图标。
#pragma once

#include <windows.h>
#include <d2d1.h>

#include <cstdint>

#include "ui/theme.h"

namespace usb::ui {

D2D1_RECT_F Rect(float left, float top, float right, float bottom);
D2D1_ROUNDED_RECT Round(const D2D1_RECT_F& rect, float radius);
D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha);

bool Contains(const D2D1_RECT_F& rect, float x, float y);
bool ContainsRounded(const D2D1_RECT_F& rect, float radius, float x, float y);

void FillRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
               const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color);
void StrokeRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                 const D2D1_RECT_F& rect, float radius,
                 const D2D1_COLOR_F& color, float width);

void DrawPanelShadow(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                     const D2D1_RECT_F& panel, float radius, bool dark);

void DrawUsbGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  float center_x, float center_y, float unit,
                  const D2D1_COLOR_F& color, float led_alpha);
void DrawDriveGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                    float center_x, float center_y, float unit,
                    const D2D1_COLOR_F& color, float led_alpha);
void DrawSsdGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  float center_x, float center_y, float unit,
                  const D2D1_COLOR_F& color, float led_alpha);

void DrawCloseGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                    const D2D1_RECT_F& box, const D2D1_COLOR_F& color,
                    float size);

void DrawEjectGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                    float center_x, float center_y, float size,
                    const D2D1_COLOR_F& color);

// 向上箭头：升级器专用图形，与程序图标一致。
void DrawUpgradeGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                      float center_x, float center_y, float size,
                      const D2D1_COLOR_F& color);

void DrawShieldGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                     float center_x, float center_y, float size,
                     const D2D1_COLOR_F& shield_color,
                     const D2D1_COLOR_F& mark_color);

void DrawSwitch(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                const D2D1_RECT_F& box, bool on, const D2D1_COLOR_F& on_color,
                const D2D1_COLOR_F& off_color, const D2D1_COLOR_F& knob_color);

void DrawCheckBox(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  const D2D1_RECT_F& box, bool checked,
                  const D2D1_COLOR_F& accent, const D2D1_COLOR_F& border,
                  const D2D1_COLOR_F& check_color);

bool CreateBitmapFromIcon(ID2D1RenderTarget* target, HICON icon,
                          ID2D1Bitmap** bitmap);

}  // namespace usb::ui
