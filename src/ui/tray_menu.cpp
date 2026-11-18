#include "ui/tray_menu.h"

#include <algorithm>
#include <cmath>
#include <windowsx.h>

#include "ui/draw.h"

namespace usb::ui {
namespace {

constexpr float kMenuWidth = 172.0f;
constexpr float kItemHeight = 34.0f;
constexpr float kMenuPad = 6.0f;
constexpr float kMenuGap = 6.0f;
constexpr int kDangerIndex = 2;

float SystemScale() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

COLORREF BlendOver(const D2D1_COLOR_F& foreground,
                   const D2D1_COLOR_F& background) {
    const float alpha = std::min(std::max(foreground.a, 0.0f), 1.0f);
    const float red = foreground.r * alpha + background.r * (1.0f - alpha);
    const float green = foreground.g * alpha + background.g * (1.0f - alpha);
    const float blue = foreground.b * alpha + background.b * (1.0f - alpha);
    return RGB(static_cast<BYTE>(red * 255.0f),
               static_cast<BYTE>(green * 255.0f),
               static_cast<BYTE>(blue * 255.0f));
}

RECT WorkAreaForPoint(POINT point) {
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor =
        MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

}  // namespace

int TrayMenu::Show(HINSTANCE instance, HWND owner, POINT screen_point,
                   const Palette& palette, float scale) {
    owner_ = owner;
    palette_ = palette;
    scale_ = scale > 0.0f ? scale : SystemScale();
    hover_ = -1;
    choice_ = -1;
    done_ = false;
    items_ = {L"显示窗口", L"设置…", L"退出"};

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &TrayMenu::WndProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"UsbAssistantTrayMenu";
        RegisterClassExW(&window_class);
        registered = true;
    }

    const int width = static_cast<int>(std::lround(kMenuWidth * scale_));
    const int height = static_cast<int>(std::lround(MenuHeight() * scale_));
    RECT work_area = WorkAreaForPoint(screen_point);
    int x = screen_point.x - width +
            static_cast<int>(std::lround(12.0f * scale_));
    int y = screen_point.y - height -
            static_cast<int>(std::lround(10.0f * scale_));
    if (x + width > work_area.right) x = work_area.right - width;
    if (x < work_area.left) x = work_area.left;
    if (y + height > work_area.bottom) y = work_area.bottom - height;
    if (y < work_area.top) y = work_area.top;

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            L"UsbAssistantTrayMenu", L"", WS_POPUP, x, y, width,
                            height, nullptr, nullptr, instance, this);
    if (!hwnd_) return -1;

    const int radius = static_cast<int>(std::lround(12.0f * scale_));
    SetWindowRgn(hwnd_,
                 CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius),
                 FALSE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);

    bool previous_down = false;
    while (!done_) {
        MSG message = {};
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            0, nullptr, 40, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                Finish(-1);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done_) break;
        if (wait != WAIT_TIMEOUT) continue;

        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                          (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        POINT cursor = {};
        GetCursorPos(&cursor);
        RECT rect = {};
        if (hwnd_) GetWindowRect(hwnd_, &rect);
        const bool inside = hwnd_ && PtInRect(&rect, cursor) != FALSE;
        if (escape || (down && !inside) || (previous_down && !down && !inside)) {
            Finish(-1);
            break;
        }
        previous_down = down;
    }
    return choice_;
}

float TrayMenu::ItemTop(int index) const {
    float top = kMenuPad + static_cast<float>(index) * kItemHeight;
    if (index >= kDangerIndex) top += kMenuGap;
    return top;
}

float TrayMenu::MenuHeight() const {
    return kMenuPad * 2.0f + kItemHeight * static_cast<float>(items_.size()) +
           kMenuGap;
}

int TrayMenu::HitItem(int y_px) const {
    const float y = static_cast<float>(y_px) / scale_;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const float top = ItemTop(i);
        if (y >= top && y <= top + kItemHeight) return i;
    }
    return -1;
}

void TrayMenu::Paint(HDC dc) {
    const int width = static_cast<int>(std::lround(kMenuWidth * scale_));
    const int height = static_cast<int>(std::lround(MenuHeight() * scale_));

    HBRUSH background = CreateSolidBrush(ToColorRef(palette_.card));
    RECT full_rect = {0, 0, width, height};
    FillRect(dc, &full_rect, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(palette_.text_primary));
    HFONT font = CreateFontW(
        -static_cast<int>(std::lround(13.0f * scale_)), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
        L"Microsoft YaHei UI");

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const float top = ItemTop(i);
        const int item_top = static_cast<int>(std::lround(top * scale_));
        const int item_bottom =
            static_cast<int>(std::lround((top + kItemHeight) * scale_));
        const int item_left = static_cast<int>(std::lround(6.0f * scale_));
        const int item_right = width - item_left;
        const bool danger = i == kDangerIndex;

        if (i == hover_) {
            const COLORREF fill =
                danger ? BlendOver(WithAlpha(palette_.danger, 0.16f),
                                   palette_.card)
                       : BlendOver(palette_.hover_fill, palette_.card);
            HBRUSH hover_brush = CreateSolidBrush(fill);
            HPEN hover_pen = CreatePen(PS_SOLID, 0, fill);
            const HGDIOBJ old_brush = SelectObject(dc, hover_brush);
            const HGDIOBJ old_pen = SelectObject(dc, hover_pen);
            const int radius = static_cast<int>(std::lround(8.0f * scale_));
            RoundRect(dc, item_left, item_top, item_right, item_bottom, radius,
                      radius);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(hover_brush);
            DeleteObject(hover_pen);
        }

        if (danger && i > 0) {
            HPEN pen = CreatePen(PS_SOLID, 1,
                                  BlendOver(palette_.card_border, palette_.card));
            const HGDIOBJ old_pen = SelectObject(dc, pen);
            const int line_y = static_cast<int>(
                std::lround((top - kMenuGap * 0.5f) * scale_));
            MoveToEx(dc, item_left + 8, line_y, nullptr);
            LineTo(dc, item_right - 8, line_y);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }

        SetTextColor(dc, ToColorRef(danger ? palette_.danger
                                          : palette_.text_primary));
        const HGDIOBJ old_font = SelectObject(dc, font);
        RECT text_rect = {item_left + static_cast<int>(12.0f * scale_),
                          item_top, item_right, item_bottom};
        DrawTextW(dc, items_[i].c_str(), -1, &text_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        SelectObject(dc, old_font);
    }
    DeleteObject(font);
}

void TrayMenu::Finish(int choice) {
    choice_ = choice;
    done_ = true;
    if (hwnd_) {
        HWND hwnd = hwnd_;
        hwnd_ = nullptr;
        if (GetCapture() == hwnd) ReleaseCapture();
        DestroyWindow(hwnd);
    }
}

LRESULT CALLBACK TrayMenu::WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    TrayMenu* self = reinterpret_cast<TrayMenu*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<TrayMenu*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT TrayMenu::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
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
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            const int hit = HitItem(GET_Y_LPARAM(lparam));
            if (hit != hover_) {
                hover_ = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP: {
            const int hit = HitItem(GET_Y_LPARAM(lparam));
            Finish(hit);
            return 0;
        }
        case WM_RBUTTONUP:
            Finish(-1);
            return 0;
        case WM_CAPTURECHANGED:
            if (!done_) Finish(-1);
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
