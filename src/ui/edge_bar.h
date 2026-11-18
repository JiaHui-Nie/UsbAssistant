// 屏幕边缘的悬浮条：主窗口贴边收起后显示，点击恢复。
#pragma once

#include <windows.h>

namespace usb::ui {

constexpr UINT kMessageEdgeBarClicked = WM_APP + 40;

class EdgeBar {
public:
    EdgeBar() = default;
    ~EdgeBar();
    EdgeBar(const EdgeBar&) = delete;
    EdgeBar& operator=(const EdgeBar&) = delete;

    void Show(HINSTANCE instance, HWND owner, HICON icon, int x, int y, int side,
              bool dark);
    void Hide();
    void Destroy();
    void SetTheme(bool dark);
    void SetIcon(HICON icon) { icon_ = icon; }
    bool visible() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);
    void Paint(HDC dc);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool dark_ = false;
    int side_ = 42;
};

}  // namespace usb::ui
