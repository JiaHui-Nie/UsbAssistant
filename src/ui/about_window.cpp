#include "ui/about_window.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "core/log.h"
#include "core/version.h"
#include "ui/draw.h"

namespace usb::ui {
namespace {

constexpr float kWidth = 440.0f;
constexpr float kHeight = 362.0f;
constexpr float kCardTop = 58.0f;
constexpr float kSidePad = 14.0f;

float SystemDpi() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) : 96.0f;
}

float WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            fn = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(user32, "GetDpiForWindow")));
        }
    }
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) return static_cast<float>(dpi);
    }
    return SystemDpi();
}

RECT WorkAreaForWindow(HWND hwnd) {
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

bool IsWebUrl(const std::wstring& url) {
    return url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0;
}

}  // namespace

AboutWindow::~AboutWindow() { ReleaseTarget(); }

bool AboutWindow::Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
                       IDWriteFactory* dwrite, const Fonts* fonts,
                       const Palette& palette, std::wstring version,
                       CheckHandler on_check, CloseHandler on_close) {
    palette_ = palette;
    version_ = std::move(version);
    on_check_ = std::move(on_check);
    on_close_ = std::move(on_close);

    if (hwnd_) {
        hover_ = Hover::None;
        dragging_ = false;
        tracking_leave_ = false;
        checking_ = false;
        has_result_ = false;
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
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
        window_class.lpfnWndProc = &AboutWindow::WndProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"UsbAssistantAbout";
        RegisterClassExW(&window_class);
        registered = true;
    }

    scale_ = WindowDpi(owner) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;

    const int width = static_cast<int>(std::lround(kWidth * scale_));
    const int height = static_cast<int>(std::lround(kHeight * scale_));
    const RECT work_area = WorkAreaForWindow(owner);
    const int left =
        work_area.left + ((work_area.right - work_area.left) - width) / 2;
    const int top =
        work_area.top + ((work_area.bottom - work_area.top) - height) / 2;

    // 不设置 owner：置顶层级由 SetTopMost 与主窗口统一维护。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"UsbAssistantAbout", L"关于",
                            WS_POPUP, left, top, width, height, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) return false;

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 FALSE);
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void AboutWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool AboutWindow::IsOpen() const { return hwnd_ != nullptr; }

void AboutWindow::SetTopMost(bool enabled) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void AboutWindow::SetPalette(const Palette& palette) {
    palette_ = palette;
    if (hwnd_) {
        const BOOL dark = palette_.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AboutWindow::SetChecking(bool checking) {
    checking_ = checking;
    if (checking) has_result_ = false;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void AboutWindow::SetUpdateResult(const UpdateCheckResult& result) {
    checking_ = false;
    result_ = result;
    has_result_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void AboutWindow::ApplyScale() {
    scale_ = WindowDpi(hwnd_) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;
    ResizeWindow();
}

void AboutWindow::ResizeWindow() {
    if (!hwnd_) return;
    const int width = static_cast<int>(std::lround(kWidth * scale_));
    const int height = static_cast<int>(std::lround(kHeight * scale_));
    RECT current = {};
    GetWindowRect(hwnd_, &current);
    SetWindowPos(hwnd_, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 TRUE);
    if (target_) target_->Resize(D2D1::SizeU(width, height));
}

bool AboutWindow::EnsureTarget() {
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

    D2D1_HWND_RENDER_TARGET_PROPERTIES properties =
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

void AboutWindow::ReleaseTarget() {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

D2D1_RECT_F AboutWindow::CloseRect() const {
    return Rect(kWidth - kSidePad - 28.0f, 12.0f, kWidth - kSidePad, 40.0f);
}

D2D1_RECT_F AboutWindow::CheckRect() const {
    return Rect(kWidth - kSidePad - 22.0f - 96.0f, 254.0f,
                kWidth - kSidePad - 22.0f, 290.0f);
}

D2D1_RECT_F AboutWindow::RepositoryLinkRect() const {
    return Rect(36.0f, 206.0f, kWidth - 36.0f, 234.0f);
}

D2D1_RECT_F AboutWindow::StatusRect() const {
    return Rect(36.0f, 292.0f, kWidth - 36.0f, 326.0f);
}

AboutWindow::Hover AboutWindow::HitControl(float x, float y) const {
    if (Contains(CloseRect(), x, y)) return Hover::Close;
    if (Contains(RepositoryLinkRect(), x, y)) return Hover::Repository;
    if (!checking_ && Contains(CheckRect(), x, y)) return Hover::Check;
    if (has_result_ &&
        result_.status == UpdateCheckResult::Status::UpdateAvailable &&
        Contains(StatusRect(), x, y)) {
        return Hover::Release;
    }
    return Hover::None;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void AboutWindow::PaintButton(ID2D1RenderTarget* target,
                              ID2D1SolidColorBrush* brush,
                              const D2D1_RECT_F& rect,
                              const std::wstring& text, bool primary,
                              bool hovered, bool enabled) {
    D2D1_COLOR_F fill = primary ? palette_.accent : palette_.secondary_button;
    D2D1_COLOR_F color = primary ? palette_.on_accent : palette_.secondary_text;
    if (!enabled) {
        fill = WithAlpha(fill, 0.55f);
        color = palette_.text_tertiary;
    } else if (hovered) {
        fill = WithAlpha(fill, 0.86f);
    }
    FillRound(target, brush, rect, 10.0f, fill);
    DrawText(target, brush, text, fonts_->button, rect, color);
}

void AboutWindow::Paint() {
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

    DrawText(target_, brush, L"关于", fonts_->title,
             Rect(kSidePad + 4.0f, 12.0f, kWidth - 60.0f, 38.0f),
             palette_.text_primary);

    const D2D1_RECT_F close = CloseRect();
    if (hover_ == Hover::Close) {
        FillRound(target_, brush, close, 13.0f,
                  WithAlpha(palette_.hover_fill, 1.0f));
    }
    DrawCloseGlyph(target_, brush, close, palette_.text_secondary, 9.0f);

    const D2D1_RECT_F card =
        Rect(kSidePad, kCardTop, kWidth - kSidePad, kHeight - kSidePad);
    FillRound(target_, brush, card, 14.0f, palette_.card);
    StrokeRound(target_, brush, card, 14.0f, palette_.card_border, 1.0f);

    // 图标卡片 + 程序名/版本
    const D2D1_RECT_F tile = Rect(36.0f, 80.0f, 92.0f, 136.0f);
    FillRound(target_, brush, tile, 14.0f, palette_.icon_tile);
    DrawUsbGlyph(target_, brush, (tile.left + tile.right) * 0.5f,
                 (tile.top + tile.bottom) * 0.5f, 17.0f, palette_.accent, 0.8f);

    DrawText(target_, brush, L"U 盘助手", fonts_->title,
             Rect(106.0f, 78.0f, kWidth - 36.0f, 106.0f),
             palette_.text_primary);
    DrawText(target_, brush, L"版本 " + version_, fonts_->meta,
             Rect(106.0f, 106.0f, kWidth - 36.0f, 128.0f),
             palette_.text_secondary);
    DrawText(target_, brush, L"USB 存储设备安全弹出与分区管理", fonts_->meta,
             Rect(106.0f, 130.0f, kWidth - 36.0f, 152.0f),
             palette_.text_tertiary);

    brush->SetColor(WithAlpha(palette_.text_primary, 0.08f));
    target_->DrawLine({36.0f, 172.0f}, {kWidth - 36.0f, 172.0f}, brush, 1.0f);

    // 项目主页
    DrawText(target_, brush, L"项目主页", fonts_->part_name,
             Rect(36.0f, 184.0f, 200.0f, 206.0f),
             palette_.text_secondary);
    const bool repo_hover = hover_ == Hover::Repository;
    DrawTextTrim(dwrite_, target_, brush, kRepositoryUrl, fonts_->meta,
                 RepositoryLinkRect(),
                 repo_hover ? palette_.accent : WithAlpha(palette_.accent, 0.9f));
    brush->SetColor(WithAlpha(palette_.accent, repo_hover ? 0.85f : 0.45f));
    target_->DrawLine({36.0f, 232.0f},
                      {36.0f + 300.0f, 232.0f}, brush, 1.0f);

    brush->SetColor(WithAlpha(palette_.text_primary, 0.08f));
    target_->DrawLine({36.0f, 244.0f}, {kWidth - 36.0f, 244.0f}, brush, 1.0f);

    // 版本更新
    DrawText(target_, brush, L"版本更新", fonts_->part_name,
             Rect(36.0f, 254.0f, 260.0f, 276.0f),
             palette_.text_secondary);

    std::wstring status_text;
    D2D1_COLOR_F status_color = palette_.text_tertiary;
    if (checking_) {
        status_text = L"正在检查更新…";
        status_color = palette_.text_secondary;
    } else if (has_result_) {
        status_text = result_.message;
        switch (result_.status) {
            case UpdateCheckResult::Status::UpToDate:
                status_color = palette_.success;
                break;
            case UpdateCheckResult::Status::UpdateAvailable:
                status_text += L"，点击查看 Release 页面";
                status_color = palette_.accent;
                break;
            case UpdateCheckResult::Status::NetworkError:
                status_color = palette_.warn;
                break;
            case UpdateCheckResult::Status::ParseError:
                status_color = palette_.danger;
                break;
        }
    } else if (kDebugBuild) {
        status_text = L"开发版：每次检查更新都会显示结果。";
    } else {
        status_text = L"点击「检查更新」获取最新版本。";
    }
    DrawTextTrim(dwrite_, target_, brush, status_text, fonts_->meta,
                 StatusRect(), status_color);

    PaintButton(target_, brush, CheckRect(),
                checking_ ? L"检查中…" : L"检查更新", false,
                hover_ == Hover::Check, !checking_);

    brush->Release();
    const HRESULT result = target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) ReleaseTarget();
}

void AboutWindow::OpenUrl(const std::wstring& url) {
    if (!IsWebUrl(url)) {
        USB_LOG_WARN(L"about", L"拒绝打开非网页链接：%s", url.c_str());
        return;
    }
    const HINSTANCE instance = ShellExecuteW(nullptr, L"open", url.c_str(),
                                            nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(instance) <= 32) {
        USB_LOG_WARN(L"about", L"打开链接失败：%s", url.c_str());
    }
}

// ---------------------------------------------------------------------------
// 消息
// ---------------------------------------------------------------------------

LRESULT CALLBACK AboutWindow::WndProc(HWND hwnd, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    AboutWindow* self = reinterpret_cast<AboutWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<AboutWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT AboutWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
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
            switch (hit) {
                case Hover::Close:
                    Close();
                    return 0;
                case Hover::Check:
                    if (!checking_ && on_check_) {
                        SetChecking(true);
                        on_check_();
                    }
                    return 0;
                case Hover::Repository:
                    OpenUrl(kRepositoryUrl);
                    return 0;
                case Hover::Release:
                    OpenUrl(result_.release_url);
                    return 0;
                case Hover::None:
                    break;
            }
            if (y < kCardTop) {
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
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            const Hover hit = HitControl(x, y);
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
            if (wparam == VK_ESCAPE) {
                Close();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            ApplyScale();
            if (RECT* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
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
        case WM_NCDESTROY: {
            hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            CloseHandler handler = std::move(on_close_);
            on_close_ = nullptr;
            if (handler) handler();
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::ui
