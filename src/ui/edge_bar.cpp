#include "ui/edge_bar.h"

#include <algorithm>
#include <cmath>

#include "ui/theme.h"

namespace usb::ui {

EdgeBar::~EdgeBar() { Destroy(); }

void EdgeBar::Show(HINSTANCE instance, HWND owner, HICON icon, int x, int y,
                   int side, bool dark) {
    owner_ = owner;
    icon_ = icon;
    dark_ = dark;
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
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                L"UsbAssistantEdgeBar", L"", WS_POPUP, x, y,
                                side_, side_, nullptr, nullptr, instance, this);
        if (!hwnd_) return;
    }

    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, side_ + 1, side_ + 1, 14, 14),
                 TRUE);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, side_, side_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
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

void EdgeBar::SetTheme(bool dark) {
    dark_ = dark;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void EdgeBar::Paint(HDC dc) {
    RECT rect = {};
    GetClientRect(hwnd_, &rect);

    const COLORREF background =
        dark_ ? RGB(0x2A, 0x2A, 0x2D) : RGB(0xFF, 0xFF, 0xFF);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);

    if (icon_) {
        DrawIconEx(dc, 0, 0, icon_, rect.right - rect.left,
                   rect.bottom - rect.top, 0, nullptr, DI_NORMAL);
    }
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
            const HDC dc = BeginPaint(hwnd, &paint);
            Paint(dc);
            EndPaint(hwnd, &paint);
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
