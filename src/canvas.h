#pragma once

#include <windows.h>
#include <d2d1.h>

// 基于 DIB + D2D DC 渲染目标的离屏画布。
// 用于分层窗口（UpdateLayeredWindow）逐像素 alpha 合成，从而自绘圆角与阴影。
class Canvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    bool Init(ID2D1Factory* factory, int widthPx, int heightPx);
    bool Resize(int widthPx, int heightPx);
    void Release();

    void Begin(float dpi);
    HRESULT End();
    void Present(HWND hwnd, unsigned char constantAlpha);

    ID2D1RenderTarget* Rt() const { return rt_; }
    const void* Bits() const { return bits_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    bool Valid() const { return rt_ != nullptr && bmp_ != nullptr; }

private:
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HBITMAP oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    ID2D1DCRenderTarget* rt_ = nullptr;
};
