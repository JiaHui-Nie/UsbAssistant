#include "ui/draw.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>

namespace usb::ui {

D2D1_RECT_F Rect(float left, float top, float right, float bottom) {
    return D2D1_RECT_F{left, top, right, bottom};
}

D2D1_ROUNDED_RECT Round(const D2D1_RECT_F& rect, float radius) {
    D2D1_ROUNDED_RECT rounded = {};
    rounded.rect = rect;
    rounded.radiusX = radius;
    rounded.radiusY = radius;
    return rounded;
}

D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha) {
    D2D1_COLOR_F out = color;
    out.a = color.a * alpha;
    return out;
}

bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top &&
           y <= rect.bottom;
}

bool ContainsRounded(const D2D1_RECT_F& rect, float radius, float x, float y) {
    if (!Contains(rect, x, y)) return false;
    if (radius <= 0.0f) return true;
    const float clamped_x = std::min(std::max(x, rect.left + radius),
                                     rect.right - radius);
    const float clamped_y = std::min(std::max(y, rect.top + radius),
                                     rect.bottom - radius);
    const float dx = x - clamped_x;
    const float dy = y - clamped_y;
    return dx * dx + dy * dy <= radius * radius;
}

void FillRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
               const D2D1_RECT_F& rect, float radius,
               const D2D1_COLOR_F& color) {
    if (!target || !brush) return;
    brush->SetColor(color);
    target->FillRoundedRectangle(Round(rect, radius), brush);
}

void StrokeRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                 const D2D1_RECT_F& rect, float radius,
                 const D2D1_COLOR_F& color, float width) {
    if (!target || !brush) return;
    brush->SetColor(color);
    target->DrawRoundedRectangle(Round(rect, radius), brush, width);
}

void DrawPanelShadow(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                     const D2D1_RECT_F& panel, float radius, bool dark) {
    if (!target || !brush) return;
    constexpr int kLayers = 18;
    constexpr float kMaxGrow = 15.0f;
    const float base_alpha = dark ? 0.038f : 0.020f;
    for (int i = kLayers - 1; i >= 0; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(kLayers - 1);
        const float grow = kMaxGrow * t;
        brush->SetColor(D2D1_COLOR_F{0.0f, 0.0f, 0.0f, base_alpha});
        const D2D1_RECT_F rect =
            Rect(panel.left - grow, panel.top - grow + 2.0f,
                 panel.right + grow, panel.bottom + grow + 2.0f);
        target->FillRoundedRectangle(Round(rect, radius + grow), brush);
    }
}

void DrawUsbGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  float center_x, float center_y, float unit,
                  const D2D1_COLOR_F& color, float led_alpha) {
    const float body_w = unit * 1.34f;
    const float body_h = unit * 0.86f;
    const float connector_w = unit * 0.44f;
    const float connector_h = unit * 0.40f;

    const float total = connector_w + 1.0f + body_w;
    const float left = center_x - total * 0.5f;
    const float top = center_y - body_h * 0.5f;

    brush->SetColor(color);
    const D2D1_RECT_F connector =
        Rect(left, center_y - connector_h * 0.5f, left + connector_w,
             center_y + connector_h * 0.5f);
    target->FillRoundedRectangle(Round(connector, connector_h * 0.24f), brush);

    const D2D1_RECT_F body =
        Rect(left + connector_w + 1.0f, top, left + total, top + body_h);
    target->FillRoundedRectangle(Round(body, body_h * 0.30f), brush);

    if (led_alpha > 0.0f) {
        brush->SetColor(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, led_alpha});
        const float led_size = body_h * 0.22f;
        const float led_x = body.right - body_h * 0.42f;
        const D2D1_RECT_F led =
            Rect(led_x - led_size * 0.5f, center_y - led_size * 0.5f,
                 led_x + led_size * 0.5f, center_y + led_size * 0.5f);
        target->FillRoundedRectangle(Round(led, led_size * 0.5f), brush);
    }
}

void DrawDriveGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                    float center_x, float center_y, float unit,
                    const D2D1_COLOR_F& color, float led_alpha) {
    const float body_w = unit * 1.46f;
    const float body_h = unit * 1.02f;
    const D2D1_RECT_F body =
        Rect(center_x - body_w * 0.5f, center_y - body_h * 0.5f,
             center_x + body_w * 0.5f, center_y + body_h * 0.5f);
    brush->SetColor(color);
    target->FillRoundedRectangle(Round(body, body_h * 0.22f), brush);

    brush->SetColor(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.55f});
    const D2D1_ELLIPSE platter = {{center_x, center_y}, body_h * 0.29f,
                                  body_h * 0.29f};
    target->DrawEllipse(platter, brush, 1.5f);
    const D2D1_ELLIPSE hub = {{center_x, center_y}, body_h * 0.10f,
                              body_h * 0.10f};
    target->FillEllipse(hub, brush);

    if (led_alpha > 0.0f) {
        brush->SetColor(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, led_alpha});
        const D2D1_ELLIPSE led = {
            {body.right - body_h * 0.26f, body.bottom - body_h * 0.22f},
            body_h * 0.09f, body_h * 0.09f};
        target->FillEllipse(led, brush);
    }
}

void DrawSsdGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  float center_x, float center_y, float unit,
                  const D2D1_COLOR_F& color, float led_alpha) {
    const float body_w = unit * 1.46f;
    const float body_h = unit * 1.02f;
    const D2D1_RECT_F body =
        Rect(center_x - body_w * 0.5f, center_y - body_h * 0.5f,
             center_x + body_w * 0.5f, center_y + body_h * 0.5f);
    brush->SetColor(color);
    target->FillRoundedRectangle(Round(body, body_h * 0.22f), brush);

    brush->SetColor(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.55f});
    const float chip_w = body_w * 0.26f;
    const float chip_h = body_h * 0.24f;
    const float gap_x = body_w * 0.10f;
    const float left = center_x - chip_w - gap_x * 0.5f;
    const float top_row = center_y - chip_h - body_h * 0.04f;
    const float bottom_row = center_y + body_h * 0.04f;
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const float x = left + column * (chip_w + gap_x);
            const float y = row == 0 ? top_row : bottom_row;
            const D2D1_RECT_F chip = Rect(x, y, x + chip_w, y + chip_h);
            target->FillRoundedRectangle(Round(chip, chip_h * 0.18f), brush);
        }
    }

    if (led_alpha > 0.0f) {
        brush->SetColor(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, led_alpha});
        const D2D1_ELLIPSE led = {
            {body.right - body_h * 0.20f, body.bottom - body_h * 0.18f},
            body_h * 0.07f, body_h * 0.07f};
        target->FillEllipse(led, brush);
    }
}

void DrawCloseGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                    const D2D1_RECT_F& box, const D2D1_COLOR_F& color,
                    float size) {
    const float center_x = (box.left + box.right) * 0.5f;
    const float center_y = (box.top + box.bottom) * 0.5f;
    brush->SetColor(color);
    const float half = size * 0.5f;
    target->DrawLine({center_x - half, center_y - half},
                     {center_x + half, center_y + half}, brush, 1.4f);
    target->DrawLine({center_x + half, center_y - half},
                     {center_x - half, center_y + half}, brush, 1.4f);
}

void DrawEjectGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                    float center_x, float center_y, float size,
                    const D2D1_COLOR_F& color) {
    if (!target || !brush || size <= 0.0f) return;
    const float half = size * 0.5f;
    const float top = center_y - half;
    const float base = center_y + size * 0.10f;
    brush->SetColor(color);
    const D2D1_POINT_2F apex = {center_x, top};
    const D2D1_POINT_2F left = {center_x - half, base};
    const D2D1_POINT_2F right = {center_x + half, base};
    target->DrawLine(apex, left, brush, 1.5f);
    target->DrawLine(left, right, brush, 1.5f);
    target->DrawLine(right, apex, brush, 1.5f);
    const float bar_y = base + size * 0.24f;
    target->DrawLine({center_x - half, bar_y}, {center_x + half, bar_y}, brush,
                     1.6f);
}

void DrawUpgradeGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                      float center_x, float center_y, float size,
                      const D2D1_COLOR_F& color) {
    if (!target || !brush || size <= 0.0f) return;
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory) return;

    // 与程序图标一致的线性箭头；size 为箭头包围盒高度。
    // 光学居中：包围盒中心比几何中心再上移约 2.4%。
    const float stroke = size * 0.1875f;
    const float optical_y = center_y - size * 0.024f;
    const float top = optical_y - size * 0.5f;
    const float tip_y = top + stroke * 0.5f;
    const float bottom_y = top + size - stroke * 0.5f;
    const float head_half = size * 0.320f;
    const float head_base_y = top + size * 0.420f;

    ID2D1PathGeometry* path = nullptr;
    if (FAILED(factory->CreatePathGeometry(&path)) || !path) return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink)) || !sink) {
        path->Release();
        return;
    }

    sink->BeginFigure({center_x, tip_y + size * 0.016f},
                      D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({center_x, bottom_y});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);

    sink->BeginFigure({center_x - head_half, head_base_y},
                      D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({center_x, tip_y});
    sink->AddLine({center_x + head_half, head_base_y});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    D2D1_STROKE_STYLE_PROPERTIES props = {};
    props.startCap = D2D1_CAP_STYLE_ROUND;
    props.endCap = D2D1_CAP_STYLE_ROUND;
    props.dashCap = D2D1_CAP_STYLE_ROUND;
    props.lineJoin = D2D1_LINE_JOIN_ROUND;
    props.miterLimit = 1.0f;
    ID2D1StrokeStyle* style = nullptr;
    factory->CreateStrokeStyle(props, nullptr, 0, &style);

    brush->SetColor(color);
    target->DrawGeometry(path, brush, stroke, style);
    if (style) style->Release();

    sink->Release();
    path->Release();
}

void DrawShieldGlyph(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                     float center_x, float center_y, float size,
                     const D2D1_COLOR_F& shield_color,
                     const D2D1_COLOR_F& mark_color) {
    if (!target || !brush || size <= 0.0f) return;
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory) return;

    ID2D1PathGeometry* path = nullptr;
    if (FAILED(factory->CreatePathGeometry(&path)) || !path) return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink)) || !sink) {
        path->Release();
        return;
    }

    const float width = size * 0.82f;
    const float top = center_y - size * 0.5f;
    const float left = center_x - width * 0.5f;
    const float right = center_x + width * 0.5f;
    sink->BeginFigure({center_x, top}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({right, top + size * 0.16f});
    sink->AddLine({right, center_y + size * 0.06f});
    sink->AddLine({center_x, center_y + size * 0.46f});
    sink->AddLine({left, center_y + size * 0.06f});
    sink->AddLine({left, top + size * 0.16f});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    brush->SetColor(shield_color);
    target->FillGeometry(path, brush);
    sink->Release();
    path->Release();

    brush->SetColor(mark_color);
    target->DrawLine({center_x - width * 0.20f, center_y - size * 0.02f},
                     {center_x - width * 0.04f, center_y + size * 0.16f},
                     brush, 1.9f);
    target->DrawLine({center_x - width * 0.04f, center_y + size * 0.16f},
                     {center_x + width * 0.24f, center_y - size * 0.20f},
                     brush, 1.9f);
}

void DrawSwitch(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                const D2D1_RECT_F& box, bool on, const D2D1_COLOR_F& on_color,
                const D2D1_COLOR_F& off_color,
                const D2D1_COLOR_F& knob_color) {
    const float height = box.bottom - box.top;
    const float radius = height * 0.5f;
    FillRound(target, brush, box, radius, on ? on_color : off_color);

    const float knob_radius = radius - 2.0f;
    const float knob_x = on ? box.right - radius : box.left + radius;
    const float center_y = (box.top + box.bottom) * 0.5f;
    brush->SetColor(knob_color);
    target->FillEllipse(D2D1_ELLIPSE{{knob_x, center_y}, knob_radius, knob_radius},
                        brush);
}

void DrawCheckBox(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  const D2D1_RECT_F& box, bool checked,
                  const D2D1_COLOR_F& accent, const D2D1_COLOR_F& border,
                  const D2D1_COLOR_F& check_color) {
    if (checked) {
        FillRound(target, brush, box, 4.0f, accent);
        brush->SetColor(check_color);
        target->DrawLine({box.left + 3.5f, (box.top + box.bottom) * 0.5f + 0.5f},
                         {box.left + 6.8f, box.bottom - 4.0f}, brush, 1.7f);
        target->DrawLine({box.left + 6.8f, box.bottom - 4.0f},
                         {box.right - 3.5f, box.top + 3.8f}, brush, 1.7f);
    } else {
        StrokeRound(target, brush, box, 4.0f, border, 1.4f);
    }
}

bool CreateBitmapFromIcon(ID2D1RenderTarget* target, HICON icon,
                          ID2D1Bitmap** bitmap) {
    if (!target || !icon || !bitmap) return false;
    *bitmap = nullptr;

    static ULONG_PTR gdiplus_token = 0;
    if (gdiplus_token == 0) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr) !=
            Gdiplus::Ok) {
            gdiplus_token = 0;
            return false;
        }
    }

    Gdiplus::Bitmap* source = Gdiplus::Bitmap::FromHICON(icon);
    if (!source) return false;
    const UINT width = source->GetWidth();
    const UINT height = source->GetHeight();
    if (width == 0 || height == 0) {
        delete source;
        return false;
    }

    const Gdiplus::Rect rect(0, 0, static_cast<INT>(width),
                             static_cast<INT>(height));
    Gdiplus::BitmapData data = {};
    if (source->LockBits(&rect, Gdiplus::ImageLockModeRead,
                         PixelFormat32bppPARGB, &data) != Gdiplus::Ok) {
        delete source;
        return false;
    }

    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    const HRESULT result =
        target->CreateBitmap(D2D1::SizeU(width, height), data.Scan0,
                             static_cast<UINT32>(data.Stride), properties,
                             bitmap);
    source->UnlockBits(&data);
    delete source;
    return SUCCEEDED(result) && *bitmap;
}

}  // namespace usb::ui
