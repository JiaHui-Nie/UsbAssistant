// 分层窗口的离屏画布：DIB + D2D DC 渲染目标 + UpdateLayeredWindow 合成。
#pragma once

#include <windows.h>
#include <d2d1.h>

namespace usb::ui {

class Canvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    bool Initialize(ID2D1Factory* factory, int width_px, int height_px);
    bool Resize(int width_px, int height_px);
    void Release();

    void Begin(float dpi);
    HRESULT End();
    void Present(HWND hwnd, unsigned char constant_alpha);

    ID2D1RenderTarget* target() const { return target_; }
    bool valid() const { return target_ != nullptr && bitmap_ != nullptr; }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP old_bitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    ID2D1DCRenderTarget* target_ = nullptr;
};

}  // namespace usb::ui
