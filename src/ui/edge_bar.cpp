#include "ui/edge_bar.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "ui/draw.h"

namespace usb::ui {
namespace {

// 悬浮条的设计基准边长：所有图形坐标都写在这个 42 单位坐标系里，
// 再按实际像素边长整体缩放，保证不同 DPI 下比例一致。
constexpr float kDesignSide = 42.0f;
constexpr float kCornerRadius = 7.0f;

// 图案比例取自 res/icon_preview.png（128px 画布）：
// 三角形底宽 58、高 36，底座 58x14、两端全圆角，两者间距约 12，
// 整体水平居中。换算到 42 单位后约为 19px 宽、20.4px 高。
constexpr float kGlyphLeft = 11.5f;
constexpr float kGlyphRight = 30.5f;
constexpr float kTriangleApexY = 11.2f;
constexpr float kTriangleBaseY = 22.7f;
constexpr float kBaseTop = 26.9f;
constexpr float kBaseBottom = 31.5f;

Gdiplus::PointF Point(float x, float y) { return Gdiplus::PointF(x, y); }

Gdiplus::Color ToGdiplus(const D2D1_COLOR_F& color) {
    const auto channel = [](float value) {
        return static_cast<BYTE>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return Gdiplus::Color(255, channel(color.r), channel(color.g),
                          channel(color.b));
}

// 圆角矩形路径：GDI+ 没有现成的圆角矩形，用四段圆弧拼接。
// 半径等于高度一半时即为两端全圆角的胶囊形。
void AddRoundedRect(Gdiplus::GraphicsPath* path, const Gdiplus::RectF& rect,
                    float radius) {
    const float diameter = radius * 2.0f;
    if (diameter <= 0.0f) {
        path->AddRectangle(rect);
        return;
    }
    path->AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path->AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter,
                 270.0f, 90.0f);
    path->AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                 diameter, diameter, 0.0f, 90.0f);
    path->AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter,
                 90.0f, 90.0f);
    path->CloseFigure();
}

// 软件图标造型的矢量还原：上方三角形 + 下方胶囊底座（弹出符号）。
void DrawAppMark(Gdiplus::Graphics* graphics, const Gdiplus::Color& color) {
    Gdiplus::SolidBrush brush(color);

    const Gdiplus::PointF triangle[] = {
        Point((kGlyphLeft + kGlyphRight) * 0.5f, kTriangleApexY),
        Point(kGlyphRight, kTriangleBaseY),
        Point(kGlyphLeft, kTriangleBaseY),
    };
    Gdiplus::GraphicsPath triangle_path;
    triangle_path.AddPolygon(triangle, 3);
    graphics->FillPath(&brush, &triangle_path);

    const float base_height = kBaseBottom - kBaseTop;
    Gdiplus::GraphicsPath base_path;
    AddRoundedRect(&base_path,
                   Gdiplus::RectF(kGlyphLeft, kBaseTop,
                                  kGlyphRight - kGlyphLeft, base_height),
                   base_height * 0.5f);
    graphics->FillPath(&brush, &base_path);
}

}  // namespace

EdgeBar::~EdgeBar() { Destroy(); }

void EdgeBar::Show(HINSTANCE instance, HWND owner, int x, int y, int side,
                   bool dark, const D2D1_COLOR_F& background) {
    owner_ = owner;
    dark_ = dark;
    background_ = background;
    side_ = std::max(32, side);

    if (!hwnd_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = &EdgeBar::WndProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
            window_class.lpszClassName = L"UsbAssistantEdgeBar";
            RegisterClassExW(&window_class);
            registered = true;
        }
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"UsbAssistantEdgeBar", L"", WS_POPUP, x, y, side_, side_, nullptr,
            nullptr, instance, this);
        if (!hwnd_) return;
    }

    // 分层窗口逐像素透明：圆角外的像素 alpha 为 0，既无残留也不会拦截点击，
    // 不需要（也无法抗锯齿的）SetWindowRgn。
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, side_, side_, SWP_NOACTIVATE);
    Present();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void EdgeBar::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void EdgeBar::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void EdgeBar::SetTheme(bool dark, const D2D1_COLOR_F& background) {
    dark_ = dark;
    background_ = background;
    if (hwnd_) Present();
}

void EdgeBar::Present() {
    if (!hwnd_ || side_ <= 0 || !EnsureGdiplus()) return;

    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HDC memory = CreateCompatibleDC(screen);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = side_;
    info.bmiHeader.biHeight = -side_;  // 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap =
        memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits,
                                  nullptr, 0)
               : nullptr;
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HBITMAP old_bitmap =
        static_cast<HBITMAP>(SelectObject(memory, bitmap));

    {
        // 直接以 PARGB 包装 DIB 像素：背景与图标全部走同一条 GDI+ 抗锯齿
        // 管线，输出即为预乘 alpha。
        Gdiplus::Bitmap canvas(side_, side_, side_ * 4,
                               PixelFormat32bppPARGB,
                               static_cast<BYTE*>(bits));
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        const float scale = static_cast<float>(side_) / kDesignSide;
        graphics.ScaleTransform(scale, scale);

        Gdiplus::GraphicsPath background_path;
        AddRoundedRect(&background_path,
                       Gdiplus::RectF(0.0f, 0.0f, kDesignSide, kDesignSide),
                       kCornerRadius);
        Gdiplus::SolidBrush background_brush(ToGdiplus(background_));
        graphics.FillPath(&background_brush, &background_path);

        // 单色图案：浅色主题用纯黑，深色主题用纯白，保证最大对比。
        const Gdiplus::Color mark_color =
            dark_ ? Gdiplus::Color(255, 255, 255, 255)
                  : Gdiplus::Color(255, 0, 0, 0);
        DrawAppMark(&graphics, mark_color);
    }

    RECT window = {};
    GetWindowRect(hwnd_, &window);
    POINT destination = {window.left, window.top};
    POINT source = {0, 0};
    SIZE size = {side_, side_};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd_, screen, &destination, &size, memory, &source, 0,
                        &blend, ULW_ALPHA);

    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK EdgeBar::WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    EdgeBar* self = reinterpret_cast<EdgeBar*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<EdgeBar*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT EdgeBar::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                               LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            Present();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONUP:
            if (owner_) PostMessageW(owner_, kMessageEdgeBarClicked, 0, 0);
            return 0;
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wparam, lparam);
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::ui
