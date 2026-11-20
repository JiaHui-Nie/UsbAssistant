#include "ui/message_dialog.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/dpi.h"
#include "ui/draw.h"

namespace usb::ui {
namespace {

constexpr float kWidth = 400.0f;
constexpr float kSidePad = 20.0f;
constexpr float kButtonWidth = 88.0f;
constexpr float kButtonHeight = 36.0f;

}  // namespace

MessageDialog::~MessageDialog() { ReleaseTarget(); }

bool MessageDialog::Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
                         IDWriteFactory* dwrite, const Fonts* fonts,
                         const Palette& palette, std::wstring title,
                         std::wstring message, std::wstring button_text,
                         bool top_most) {
    palette_ = palette;
    title_ = std::move(title);
    message_ = std::move(message);
    button_text_ = std::move(button_text);

    if (!fonts || !dwrite || !d2d) return false;

    const float measured = MeasureWrappedHeight(
        dwrite, fonts->toast, message_, kWidth - kSidePad * 2.0f);
    message_height_ = std::max(18.0f, measured);

    if (hwnd_) {
        hover_ = Hover::None;
        dragging_ = false;
        SetWindowPos(hwnd_, top_most ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                     0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    instance_ = instance;
    d2d_ = d2d;
    dwrite_ = dwrite;
    fonts_ = fonts;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &MessageDialog::WndProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"UsbAssistantMessage";
        RegisterClassExW(&window_class);
        registered = true;
    }

    scale_ = WindowDpi(owner) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;

    const int width = static_cast<int>(std::lround(Width() * scale_));
    const int height = static_cast<int>(std::lround(Height() * scale_));
    const POINT origin =
        CenterInWorkArea(ReferenceWorkArea(owner), width, height);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"UsbAssistantMessage",
                            title_.c_str(), WS_POPUP, origin.x, origin.y, width,
                            height, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 FALSE);
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    if (top_most) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
    }

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void MessageDialog::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

float MessageDialog::Width() const { return kWidth; }

float MessageDialog::Height() const {
    return ButtonRect().bottom + 18.0f;
}

D2D1_RECT_F MessageDialog::MessageRect() const {
    return Rect(kSidePad, 52.0f, kWidth - kSidePad, 52.0f + message_height_);
}

D2D1_RECT_F MessageDialog::ButtonRect() const {
    const float top = 52.0f + message_height_ + 16.0f;
    return Rect(kWidth - kSidePad - kButtonWidth, top, kWidth - kSidePad,
                top + kButtonHeight);
}

D2D1_RECT_F MessageDialog::CloseRect() const {
    return Rect(kWidth - 14.0f - 28.0f, 12.0f, kWidth - 14.0f, 40.0f);
}

MessageDialog::Hover MessageDialog::HitControl(float x, float y) const {
    if (Contains(CloseRect(), x, y)) return Hover::Close;
    if (Contains(ButtonRect(), x, y)) return Hover::Button;
    return Hover::None;
}

void MessageDialog::ApplyScale() {
    scale_ = WindowDpi(hwnd_) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;
    ResizeWindow();
}

void MessageDialog::ResizeWindow() {
    if (!hwnd_) return;
    const int width = static_cast<int>(std::lround(Width() * scale_));
    const int height = static_cast<int>(std::lround(Height() * scale_));
    RECT current = {};
    GetWindowRect(hwnd_, &current);
    SetWindowPos(hwnd_, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 TRUE);
    if (target_) target_->Resize(D2D1::SizeU(static_cast<UINT32>(width),
                                             static_cast<UINT32>(height)));
}

bool MessageDialog::EnsureTarget() {
    if (!d2d_ || !hwnd_) return false;
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const UINT32 width =
        static_cast<UINT32>(std::max<LONG>(1, client.right - client.left));
    const UINT32 height =
        static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top));
    const D2D1_SIZE_U size = D2D1::SizeU(width, height);
    if (target_) {
        const D2D1_SIZE_U current = target_->GetPixelSize();
        if (current.width != size.width || current.height != size.height) {
            target_->Resize(size);
        }
        return true;
    }

    const D2D1_HWND_RENDER_TARGET_PROPERTIES properties =
        D2D1::HwndRenderTargetProperties(hwnd_, size);
    const D2D1_RENDER_TARGET_PROPERTIES render_properties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(d2d_->CreateHwndRenderTarget(render_properties, properties,
                                            &target_))) {
        target_ = nullptr;
        return false;
    }
    return true;
}

void MessageDialog::ReleaseTarget() {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
}

void MessageDialog::Paint() {
    if (!EnsureTarget() || !fonts_) return;
    target_->SetDpi(96.0f * scale_, 96.0f * scale_);
    target_->BeginDraw();
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->Clear(palette_.panel);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(target_->CreateSolidColorBrush(palette_.text_primary, &brush)) ||
        !brush) {
        target_->EndDraw();
        return;
    }

    const D2D1_RECT_F frame = Rect(0.5f, 0.5f, Width() - 0.5f,
                                   Height() - 0.5f);
    StrokeRound(target_, brush, frame, 16.0f, palette_.panel_border, 1.0f);

    DrawText(target_, brush, title_, fonts_->title,
             Rect(kSidePad, 12.0f, kWidth - 60.0f, 38.0f),
             palette_.text_primary);

    const D2D1_RECT_F close = CloseRect();
    if (hover_ == Hover::Close) {
        FillRound(target_, brush, close, 13.0f, WithAlpha(palette_.hover_fill, 1.0f));
    }
    DrawCloseGlyph(target_, brush, close, palette_.text_secondary, 9.0f);

    DrawText(target_, brush, message_, fonts_->toast, MessageRect(),
             palette_.text_secondary);

    const D2D1_RECT_F button = ButtonRect();
    const bool hovered = hover_ == Hover::Button;
    FillRound(target_, brush, button, 8.0f,
              hovered ? WithAlpha(palette_.secondary_button, 0.82f)
                      : palette_.secondary_button);
    DrawText(target_, brush, button_text_, fonts_->button, button,
             palette_.secondary_text);

    brush->Release();
    const HRESULT result = target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) ReleaseTarget();
}

LRESULT CALLBACK MessageDialog::WndProc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    MessageDialog* self = reinterpret_cast<MessageDialog*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<MessageDialog*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT MessageDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            BeginPaint(hwnd, &paint);
            Paint();
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN: {
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            const Hover hit = HitControl(x, y);
            if (hit == Hover::Close || hit == Hover::Button) {
                Close();
                return 0;
            }
            if (y < 50.0f) {
                dragging_ = true;
                SetCapture(hwnd);
                GetCursorPos(&drag_cursor_start_);
                GetWindowRect(hwnd, &drag_window_start_);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if (dragging_) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                SetWindowPos(hwnd, nullptr,
                             drag_window_start_.left +
                                 (cursor.x - drag_cursor_start_.x),
                             drag_window_start_.top +
                                 (cursor.y - drag_cursor_start_.y),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (!tracking_leave_) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                tracking_leave_ = true;
            }
            const Hover hit = HitControl(GET_X_LPARAM(lparam) / scale_,
                                         GET_Y_LPARAM(lparam) / scale_);
            if (hit != hover_) {
                hover_ = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr,
                                  hit != Hover::None ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            hover_ = Hover::None;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_) {
                dragging_ = false;
                hover_ = Hover::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE || wparam == VK_RETURN ||
                wparam == VK_SPACE) {
                Close();
                return 0;
            }
            break;
        case WM_DPICHANGED:
            ApplyScale();
            if (RECT* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ReleaseTarget();
            return 0;
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::ui
