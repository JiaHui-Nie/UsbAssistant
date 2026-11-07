#include "canvas.h"

Canvas::~Canvas() { Release(); }

bool Canvas::Init(ID2D1Factory* factory, int widthPx, int heightPx) {
    if (!factory) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = {};
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    if (FAILED(factory->CreateDCRenderTarget(&props, &rt_))) {
        rt_ = nullptr;
        return false;
    }
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) return false;
    return Resize(widthPx, heightPx);
}

bool Canvas::Resize(int widthPx, int heightPx) {
    if (!rt_ || !dc_) return false;
    if (widthPx <= 0 || heightPx <= 0) return false;
    if (bmp_ && widthPx == width_ && heightPx == height_) return true;

    if (bmp_) {
        SelectObject(dc_, oldBmp_);
        DeleteObject(bmp_);
        bmp_ = nullptr;
        oldBmp_ = nullptr;
        bits_ = nullptr;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = widthPx;
    bi.bmiHeader.biHeight = -heightPx; // 自上而下，便于直接交给 D2D
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp_ || !bits) {
        if (bmp_) {
            DeleteObject(bmp_);
            bmp_ = nullptr;
        }
        return false;
    }
    bits_ = bits;
    oldBmp_ = static_cast<HBITMAP>(SelectObject(dc_, bmp_));

    RECT rect = {0, 0, widthPx, heightPx};
    if (FAILED(rt_->BindDC(dc_, &rect))) return false;

    width_ = widthPx;
    height_ = heightPx;
    return true;
}

void Canvas::Release() {
    if (rt_) {
        rt_->Release();
        rt_ = nullptr;
    }
    if (bmp_) {
        if (dc_ && oldBmp_) SelectObject(dc_, oldBmp_);
        DeleteObject(bmp_);
        bmp_ = nullptr;
        oldBmp_ = nullptr;
    }
    if (dc_) {
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    bits_ = nullptr;
    width_ = height_ = 0;
}

void Canvas::Begin(float dpi) {
    if (!rt_) return;
    rt_->SetDpi(dpi, dpi);
    rt_->BeginDraw();
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F identity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    rt_->SetTransform(&identity);
    D2D1_COLOR_F transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    rt_->Clear(&transparent);
}

HRESULT Canvas::End() {
    if (!rt_) return E_FAIL;
    return rt_->EndDraw();
}

void Canvas::Present(HWND hwnd, unsigned char constantAlpha) {
    if (!dc_ || !bmp_ || !hwnd) return;

    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    POINT dst = {rc.left, rc.top};
    POINT src = {0, 0};
    SIZE size = {width_, height_};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = constantAlpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd, screen, &dst, &size, dc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}
