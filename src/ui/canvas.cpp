#include "ui/canvas.h"

namespace usb::ui {

Canvas::~Canvas() { Release(); }

bool Canvas::Initialize(ID2D1Factory* factory, int width_px, int height_px) {
    if (!factory) return false;

    D2D1_RENDER_TARGET_PROPERTIES properties = {};
    properties.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    properties.dpiX = 96.0f;
    properties.dpiY = 96.0f;
    properties.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    if (FAILED(factory->CreateDCRenderTarget(&properties, &target_))) {
        target_ = nullptr;
        return false;
    }
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) return false;
    return Resize(width_px, height_px);
}

bool Canvas::Resize(int width_px, int height_px) {
    if (!target_ || !dc_) return false;
    if (width_px <= 0 || height_px <= 0) return false;
    if (bitmap_ && width_px == width_ && height_px == height_) return true;

    if (bitmap_) {
        SelectObject(dc_, old_bitmap_);
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        old_bitmap_ = nullptr;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_px;
    info.bmiHeader.biHeight = -height_px;  // 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_ || !bits) {
        if (bitmap_) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        return false;
    }
    old_bitmap_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));

    RECT rect = {0, 0, width_px, height_px};
    if (FAILED(target_->BindDC(dc_, &rect))) {
        SelectObject(dc_, old_bitmap_);
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        old_bitmap_ = nullptr;
        return false;
    }

    width_ = width_px;
    height_ = height_px;
    return true;
}

void Canvas::Release() {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
    if (bitmap_) {
        if (dc_ && old_bitmap_) SelectObject(dc_, old_bitmap_);
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        old_bitmap_ = nullptr;
    }
    if (dc_) {
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

void Canvas::Begin(float dpi) {
    if (!target_) return;
    target_->SetDpi(dpi, dpi);
    target_->BeginDraw();
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const D2D1_MATRIX_3X2_F identity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    target_->SetTransform(&identity);
    const D2D1_COLOR_F transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    target_->Clear(&transparent);
}

HRESULT Canvas::End() {
    if (!target_) return E_FAIL;
    return target_->EndDraw();
}

void Canvas::Present(HWND hwnd, unsigned char constant_alpha) {
    if (!dc_ || !bitmap_ || !hwnd) return;

    RECT rect = {};
    GetWindowRect(hwnd, &rect);
    POINT destination = {rect.left, rect.top};
    POINT source = {0, 0};
    SIZE size = {width_, height_};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = constant_alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd, screen, &destination, &size, dc_, &source, 0,
                        &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

}  // namespace usb::ui
