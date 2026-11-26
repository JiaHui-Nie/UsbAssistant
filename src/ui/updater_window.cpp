#include "ui/updater_window.h"

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
constexpr float kHeight = 252.0f;
constexpr float kIconSize = 56.0f;
constexpr float kIconTop = 24.0f;
constexpr float kTitleTop = 92.0f;
constexpr float kTitleBottom = 116.0f;
constexpr float kBodyBottom = kHeight - 24.0f;
constexpr float kButtonHeight = 32.0f;
constexpr float kButtonTop = 196.0f;
constexpr float kButtonRight = kWidth - 28.0f;
constexpr float kSingleWidth = 88.0f;
constexpr float kPrimaryWidth = 96.0f;
constexpr float kSecondaryWidth = 84.0f;
constexpr float kProgressHeight = 6.0f;
constexpr float kDragTop = 96.0f;

constexpr UINT_PTR kTimerProgress = 1;
constexpr UINT_PTR kTimerClose = 2;

constexpr float kPi = 3.14159265f;

}  // namespace

UpdaterWindow::~UpdaterWindow() { ReleaseTarget(); }

bool UpdaterWindow::Open(HINSTANCE instance, HWND reference, bool visible,
                         ID2D1Factory* d2d, IDWriteFactory* dwrite,
                         const Fonts* fonts, const Palette& palette,
                         ActionHandler on_update, CloseHandler on_close) {
    if (!d2d || !dwrite || !fonts) return false;
    palette_ = palette;
    on_update_ = std::move(on_update);
    on_close_ = std::move(on_close);

    instance_ = instance;
    d2d_ = d2d;
    dwrite_ = dwrite;
    fonts_ = fonts;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &UpdaterWindow::WndProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 升级器现在有任务栏按钮，使用自己的 updater.ico，避免和主程序混淆。
        window_class.hIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0));
        window_class.hIconSm = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
        window_class.lpszClassName = kUpdaterWindowClass;
        RegisterClassExW(&window_class);
        registered = true;
    }

    scale_ = WindowDpi(reference) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;

    const int width = static_cast<int>(std::lround(kWidth * scale_));
    const int height = static_cast<int>(std::lround(kHeight * scale_));
    const POINT origin =
        CenterInWorkArea(ReferenceWorkArea(reference), width, height);

    // 普通顶层窗口（不再是 WS_EX_TOOLWINDOW）：拥有独立任务栏按钮，
    // 并使用 updater.ico 与主程序区分。窗口始终置顶。
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, kUpdaterWindowClass, L"检查更新",
                            WS_POPUP, origin.x, origin.y, width, height,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 FALSE);
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    // 始终处于置顶层，与主程序设置无关。
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (visible) Show();
    return true;
}

void UpdaterWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void UpdaterWindow::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    // 启动即前置：先临时置顶。SetWindowPos 的 z 序调整不受前台锁限制，
    // 即使后面激活失败，窗口也已经在所有非置顶窗口之前。
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // 系统只允许前台进程激活窗口；附加到当前前台窗口的输入线程后再调用
    // SetForegroundWindow，是绕开前台锁的常规做法。
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD current_thread = GetCurrentThreadId();
    const bool attached =
        foreground_thread != 0 && foreground_thread != current_thread &&
        AttachThreadInput(foreground_thread, current_thread, TRUE) != FALSE;
    SetForegroundWindow(hwnd_);
    if (attached) {
        AttachThreadInput(foreground_thread, current_thread, FALSE);
    }
    // 升级器始终置顶：前置完成后直接回到置顶层，不再跟随主程序设置。
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UpdaterWindow::SetStage(UpdaterStage stage, std::wstring text) {
    stage_ = stage;
    text_ = std::move(text);
    // 阶段切换后进度重新开始；需要显示进度的调用方随后再 SetProgress。
    progress_ = -1.0f;
    UpdateAnimationTimer();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void UpdaterWindow::SetProgress(float fraction) {
    progress_ = fraction;
    UpdateAnimationTimer();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void UpdaterWindow::CloseLater(int delay_ms) {
    if (!hwnd_) return;
    SetTimer(hwnd_, kTimerClose, static_cast<UINT>(std::max(delay_ms, 0)),
             nullptr);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

D2D1_RECT_F UpdaterWindow::CloseRect() const {
    return Rect(kWidth - 14.0f - 28.0f, 12.0f, kWidth - 14.0f, 40.0f);
}

D2D1_RECT_F UpdaterWindow::PrimaryRect() const {
    const float width = HasSecondary() ? kPrimaryWidth : kSingleWidth;
    return Rect(kButtonRight - width, kButtonTop, kButtonRight,
                kButtonTop + kButtonHeight);
}

D2D1_RECT_F UpdaterWindow::SecondaryRect() const {
    return Rect(PrimaryRect().left - 10.0f - kSecondaryWidth, kButtonTop,
                PrimaryRect().left - 10.0f, kButtonTop + kButtonHeight);
}

// 标题与底部按钮之间的空白；没有按钮时延伸到窗口底部留白处。
// 状态文字与进度条都在这块区域的垂直中心，二者互斥。
D2D1_RECT_F UpdaterWindow::BodyRect() const {
    const float bottom = HasPrimary() ? kButtonTop : kBodyBottom;
    return Rect(36.0f, kTitleBottom, kWidth - 36.0f, bottom);
}

bool UpdaterWindow::HasPrimary() const {
    switch (stage_) {
        case UpdaterStage::CheckFailed:
        case UpdaterStage::UpToDate:
        case UpdaterStage::Available:
        case UpdaterStage::Failed:
            return true;
        default:
            return false;
    }
}

bool UpdaterWindow::HasSecondary() const {
    return stage_ == UpdaterStage::Available;
}

bool UpdaterWindow::ShowsProgress() const {
    return stage_ == UpdaterStage::Downloading ||
           stage_ == UpdaterStage::Updating;
}

std::wstring UpdaterWindow::PrimaryText() const {
    return stage_ == UpdaterStage::Available ? L"立即更新" : L"好的";
}

std::wstring UpdaterWindow::SecondaryText() const { return L"忽略"; }

UpdaterWindow::Hover UpdaterWindow::HitControl(float x, float y) const {
    if (Contains(CloseRect(), x, y)) return Hover::Close;
    if (HasPrimary() && Contains(PrimaryRect(), x, y)) return Hover::Primary;
    if (HasSecondary() && Contains(SecondaryRect(), x, y)) {
        return Hover::Secondary;
    }
    return Hover::None;
}

// ---------------------------------------------------------------------------
// 缩放与绘制目标
// ---------------------------------------------------------------------------

void UpdaterWindow::ApplyScale() {
    scale_ = WindowDpi(hwnd_) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;
    ResizeWindow();
}

void UpdaterWindow::ResizeWindow() {
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
    if (target_) target_->Resize(D2D1::SizeU(static_cast<UINT32>(width),
                                             static_cast<UINT32>(height)));
}

bool UpdaterWindow::EnsureTarget() {
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

void UpdaterWindow::ReleaseTarget() {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
}

void UpdaterWindow::UpdateAnimationTimer() {
    if (!hwnd_) return;
    const bool need = ShowsProgress() && progress_ < 0.0f;
    if (need && !animating_) {
        animation_start_ = GetTickCount64();
        SetTimer(hwnd_, kTimerProgress, 16, nullptr);
        animating_ = true;
    } else if (!need && animating_) {
        KillTimer(hwnd_, kTimerProgress);
        animating_ = false;
    }
}

void UpdaterWindow::PaintButton(ID2D1RenderTarget* target,
                                ID2D1SolidColorBrush* brush,
                                const D2D1_RECT_F& rect,
                                const std::wstring& text, bool primary,
                                bool hovered) {
    if (primary) {
        const D2D1_COLOR_F fill = hovered
                                      ? WithAlpha(palette_.accent, 0.86f)
                                      : palette_.accent;
        FillRound(target, brush, rect, 8.0f, fill);
        DrawText(target, brush, text, fonts_->button, rect,
                 palette_.on_accent);
        return;
    }
    // 次要按钮：透明底 + 描边，与主按钮形成明确主次。
    FillRound(target, brush, rect, 8.0f,
              hovered ? WithAlpha(palette_.hover_fill, 1.0f)
                      : WithAlpha(palette_.secondary_button, 0.55f));
    StrokeRound(target, brush, rect, 8.0f, palette_.card_border, 1.0f);
    DrawText(target, brush, text, fonts_->button, rect,
             palette_.secondary_text);
}

void UpdaterWindow::PaintProgress(ID2D1RenderTarget* target,
                                  ID2D1SolidColorBrush* brush) {
    const D2D1_RECT_F body = BodyRect();
    // 下载、解压与替换阶段的说明文字放在进度条上方。
    if (!text_.empty()) {
        DrawText(target, brush, text_, fonts_->dialog_message,
                 Rect(28.0f, body.top, kWidth - 28.0f, body.top + 30.0f),
                 palette_.text_primary);
    }
    const float center = (body.top + body.bottom) * 0.5f + 12.0f;
    const D2D1_RECT_F track =
        Rect(36.0f, center - kProgressHeight * 0.5f, kWidth - 36.0f,
             center + kProgressHeight * 0.5f);
    FillRound(target, brush, track, kProgressHeight * 0.5f,
              WithAlpha(palette_.track, 1.0f));

    const float track_width = track.right - track.left;
    if (progress_ >= 0.0f) {
        const float fraction = std::clamp(progress_, 0.0f, 1.0f);
        const float width = std::max(4.0f, track_width * fraction);
        FillRound(target, brush,
                  Rect(track.left, track.top, track.left + width, track.bottom),
                  kProgressHeight * 0.5f, palette_.accent);
        return;
    }

    // 长度未知：一条短色块左右往复，同时有节奏地缩短变长。
    const float elapsed =
        static_cast<float>(GetTickCount64() - animation_start_) / 1000.0f;
    const float travel_phase = std::fmod(elapsed / 1.6f, 1.0f);
    const float travel = 0.5f - 0.5f * std::cos(travel_phase * 2.0f * kPi);
    const float pulse =
        0.16f + 0.07f * std::sin(elapsed * 2.0f * kPi / 0.9f);
    const float width = track_width * pulse;
    const float left = track.left + (track_width - width) * travel;
    FillRound(target, brush, Rect(left, track.top, left + width, track.bottom),
              kProgressHeight * 0.5f, palette_.accent);
}

void UpdaterWindow::Paint() {
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

    // macOS 风格：内容居中，按钮右下角对齐。
    const D2D1_RECT_F close = CloseRect();
    if (hover_ == Hover::Close) {
        FillRound(target_, brush, close, 13.0f,
                  WithAlpha(palette_.hover_fill, 1.0f));
    }
    DrawCloseGlyph(target_, brush, close, palette_.text_secondary, 9.0f);

    const float icon_left = (kWidth - kIconSize) * 0.5f;
    const D2D1_RECT_F tile =
        Rect(icon_left, kIconTop, icon_left + kIconSize, kIconTop + kIconSize);
    FillRound(target_, brush, tile, 14.0f, palette_.icon_tile);
    DrawUpgradeGlyph(target_, brush, (tile.left + tile.right) * 0.5f,
                     (tile.top + tile.bottom) * 0.5f, 28.0f,
                     palette_.accent);

    DrawText(target_, brush, L"检查更新", fonts_->empty_title,
             Rect(24.0f, kTitleTop, kWidth - 24.0f, kTitleBottom),
             palette_.text_primary);

    D2D1_COLOR_F text_color = palette_.text_primary;
    switch (stage_) {
        case UpdaterStage::CheckFailed:
        case UpdaterStage::Failed:
            text_color = palette_.danger;
            break;
        case UpdaterStage::UpToDate:
            text_color = palette_.success;
            break;
        case UpdaterStage::Available:
            text_color = palette_.accent;
            break;
        default:
            break;
    }
    // 状态文字与进度条共用同一块居中区域，同一时刻只显示其中一个。
    if (ShowsProgress()) {
        PaintProgress(target_, brush);
    } else {
        DrawText(target_, brush, text_, fonts_->dialog_message, BodyRect(),
                 text_color);
    }

    if (HasSecondary()) {
        PaintButton(target_, brush, SecondaryRect(), SecondaryText(), false,
                    hover_ == Hover::Secondary);
    }
    if (HasPrimary()) {
        PaintButton(target_, brush, PrimaryRect(), PrimaryText(), true,
                    hover_ == Hover::Primary);
    }

    brush->Release();
    const HRESULT result = target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) ReleaseTarget();
}

// ---------------------------------------------------------------------------
// 消息
// ---------------------------------------------------------------------------

LRESULT CALLBACK UpdaterWindow::WndProc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    UpdaterWindow* self = reinterpret_cast<UpdaterWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<UpdaterWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT UpdaterWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    // 单实例转交：另一个升级器进程启动后通过注册消息唤醒本实例。
    static const UINT wake_message =
        RegisterWindowMessageW(kUpdaterWakeMessageName);
    if (wake_message != 0 && message == wake_message) {
        if (on_wake_) {
            on_wake_(wparam == kUpdaterWakeAutocheck);
        }
        return 0;
    }
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
            if (hit == Hover::Close) {
                Close();
                return 0;
            }
            if (hit == Hover::Primary && stage_ == UpdaterStage::Available) {
                if (on_update_) on_update_();
                return 0;
            }
            if (hit == Hover::Primary || hit == Hover::Secondary) {
                Close();
                return 0;
            }
            if (y < kDragTop) {
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
            if (wparam == VK_ESCAPE) {
                Close();
                return 0;
            }
            if (wparam == VK_RETURN && stage_ == UpdaterStage::Available) {
                if (on_update_) on_update_();
                return 0;
            }
            if (wparam == VK_RETURN && HasPrimary()) {
                Close();
                return 0;
            }
            break;
        case WM_TIMER:
            if (wparam == kTimerProgress) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == kTimerClose) {
                KillTimer(hwnd, kTimerClose);
                DestroyWindow(hwnd);
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
        case WM_THEMECHANGED: {
            // 与主程序一致：跟随系统明暗主题与强调色切换。
            palette_ = MakePalette(IsSystemDarkMode());
            const BOOL dark = palette_.dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ReleaseTarget();
            PostQuitMessage(0);
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
