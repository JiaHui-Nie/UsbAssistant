// 屏幕边缘的悬浮条：主窗口贴边收起后显示，点击恢复。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>

namespace usb::ui {

constexpr UINT kMessageEdgeBarClicked = WM_APP + 40;

class EdgeBar {
public:
    EdgeBar() = default;
    ~EdgeBar();
    EdgeBar(const EdgeBar&) = delete;
    EdgeBar& operator=(const EdgeBar&) = delete;

    void Show(HINSTANCE instance, HWND owner, int x, int y, int side, bool dark,
              const D2D1_COLOR_F& background);
    void Hide();
    void Destroy();
    void SetTheme(bool dark, const D2D1_COLOR_F& background);
    bool visible() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);
    void Present();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    bool dark_ = false;
    D2D1_COLOR_F background_ = D2D1::ColorF(D2D1::ColorF::White);
    int side_ = 42;
};

}  // namespace usb::ui
