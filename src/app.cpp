#include "app.h"
#include "render.h"
#include "eject.h"
#include "settings_window.h"
#include "tray_menu.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dbt.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace ui {
constexpr float kShadowPad = 15.0f;
constexpr float kPanelW = 372.0f;
constexpr float kHeaderH = 52.0f;
constexpr float kPadX = 14.0f;
constexpr float kPadTop = 8.0f;
constexpr float kPadBottom = 14.0f;
constexpr float kCardGap = 10.0f;
constexpr float kCardHeaderH = 64.0f;
constexpr float kPartH = 62.0f;
constexpr float kCardBottom = 10.0f;
constexpr float kEmptyH = 176.0f;
constexpr float kBtnH = 26.0f;
constexpr float kOpenW = 56.0f;
constexpr float kEjectW = 88.0f;
constexpr float kPanelRadius = 16.0f;

constexpr UINT kTimerAnim = 1;
constexpr UINT kTimerRefresh = 2;
constexpr UINT kTimerToast = 3;
constexpr UINT kTimerPoll = 4;
constexpr UINT kTimerEmptyHide = 5;
constexpr UINT kTimerBusyScan = 6;
} // namespace ui

static float CardHeight(const DeviceInfo& device) {
    return ui::kCardHeaderH + ui::kPartH * static_cast<float>(device.parts.size()) +
           ui::kCardBottom;
}

static const GUID kIidD2D1Factory = {
    0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
static const GUID kIidDWriteFactory = {
    0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

static float SystemDpi() {
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) : 96.0f;
}

static float WindowDpi(HWND hwnd) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(user32, "GetDpiForWindow"));
        }
    }
    if (fn && hwnd) {
        UINT dpi = fn(hwnd);
        if (dpi > 0) return static_cast<float>(dpi);
    }
    return SystemDpi();
}

static void EnableDpiAwareness() {
    typedef BOOL(WINAPI * SetDpiContextFn)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetDpiContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            if (fn(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) return;
        }
        auto legacy = reinterpret_cast<BOOL(WINAPI*)()>(
            GetProcAddress(user32, "SetProcessDPIAware"));
        if (legacy) legacy();
    }
}

static bool InRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

static bool InRoundedRect(const D2D1_RECT_F& r, float radius, float x, float y) {
    if (!InRect(r, x, y)) return false;
    if (radius <= 0.0f) return true;
    float cx = std::min(std::max(x, r.left + radius), r.right - radius);
    float cy = std::min(std::max(y, r.top + radius), r.bottom - radius);
    float dx = x - cx;
    float dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

bool App::Init(HINSTANCE hinst, bool startHidden) {
    hinst_ = hinst;
    EnableDpiAwareness();
    dpi_ = SystemDpi();
    scale_ = dpi_ / 96.0f;
    settings_ = LoadSettings();
    palette_ = MakePalette(QuerySystemDarkMode());

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, kIidD2D1Factory,
                                nullptr, reinterpret_cast<void**>(d2d_.Put()))) ||
        !d2d_) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIidDWriteFactory,
                                   reinterpret_cast<IUnknown**>(dwrite_.Put()))) ||
        !dwrite_) {
        return false;
    }
    if (!InitFonts(dwrite_, &fonts_)) return false;

    iconLarge_ = static_cast<HICON>(LoadImageW(hinst_, MAKEINTRESOURCEW(1),
                                               IMAGE_ICON, 32, 32, 0));
    iconSmall_ = static_cast<HICON>(LoadImageW(hinst_, MAKEINTRESOURCEW(1),
                                               IMAGE_ICON, 16, 16, 0));
    // 边缘悬浮条铺满显示，取更大尺寸避免拉伸模糊
    iconEdge_ = static_cast<HICON>(LoadImageW(hinst_, MAKEINTRESOURCEW(1),
                                              IMAGE_ICON, 64, 64, 0));
    if (!iconEdge_) iconEdge_ = iconLarge_;

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::StaticWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"UsbAssistantWindow";
    wc.hIcon = iconLarge_;
    wc.hIconSm = iconSmall_;
    RegisterClassExW(&wc);

    // 首次枚举必须同步完成，窗口还没创建，后台线程无法回传
    refreshState_ = std::make_shared<RefreshState>();
    ApplyDevices(EnumerateRemovableDevices());
    ComputeTargetPanelHeight();
    panelH_ = targetPanelH_;
    RecalcLayout();

    int widthPx = static_cast<int>(std::lround(layout_.windowW * scale_));
    int heightPx = static_cast<int>(std::lround(layout_.windowH * scale_));
    POINT origin = DefaultWindowOrigin(widthPx, heightPx);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (settings_.topMost) exStyle |= WS_EX_TOPMOST;
    hwnd_ = CreateWindowExW(exStyle, L"UsbAssistantWindow", L"U 盘助手", WS_POPUP,
                            origin.x, origin.y, widthPx, heightPx, nullptr, nullptr,
                            hinst_, this);
    if (!hwnd_) return false;

    canvas_.Init(d2d_, widthPx, heightPx);

    dpi_ = WindowDpi(hwnd_);
    scale_ = dpi_ / 96.0f;
    ComputeTargetPanelHeight();
    panelH_ = targetPanelH_;
    RecalcLayout();

    PaintNow();
    if (startHidden) {
        // 开机自启（--autostart）：直接收进托盘，不显示窗口
        inTray_ = true;
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        ApplyTopMost();
        if (devices_.empty()) ArmEmptyHideTimer();
    }

    AddTrayIcon();

    // Shell 通知：文件复制导致可用空间变化、介质插入/移除、盘符增删时触发刷新
    SHChangeNotifyEntry notifyEntry = {};
    notifyEntry.pidl = nullptr;
    notifyEntry.fRecursive = TRUE;
    shellNotifyId_ = SHChangeNotifyRegister(
        hwnd_, SHCNRF_ShellLevel,
        SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED |
            SHCNE_MEDIAREMOVED | SHCNE_FREESPACE,
        WM_APP_SHELL_NOTIFY, 1, &notifyEntry);

    ApplyRefreshTimer();

    if (startHidden) {
        windowAlpha_ = 1.0f;
    } else {
        windowAlpha_ = 0.0f;
        StartAnim();
    }
    return true;
}

int App::Run() {
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            PaintNow();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float x = pt.x / scale_;
            float y = pt.y / scale_;
            if (InRoundedRect(layout_.panel, ui::kPanelRadius, x, y)) return HTCLIENT;
            return HTTRANSPARENT;
        }

        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) / scale_;
            float y = GET_Y_LPARAM(lp) / scale_;
            if (emptyHideArmed_ && devices_.empty()) ArmEmptyHideTimer();
            HitResult hit = HitTest(x, y);
            if (hit.kind == HitKind::ScrollBar) {
                // 滚动条：开始拖动；若点空轨道则先把滑块移到鼠标处
                scrollDragging_ = true;
                pressed_ = hit;
                D2D1_RECT_F thumb = ScrollThumbRect();
                if (y < thumb.top || y > thumb.bottom) {
                    float trackH =
                        layout_.contentClip.bottom - layout_.contentClip.top;
                    float thumbH = ScrollThumbHeight();
                    float travel = std::max(1.0f, trackH - thumbH);
                    float progress =
                        (y - layout_.contentClip.top - thumbH * 0.5f) / travel;
                    progress = std::min(std::max(progress, 0.0f), 1.0f);
                    scrollTarget_ = progress * layout_.scrollMax;
                }
                scrollDragStartY_ = y;
                scrollDragStartTarget_ = scrollTarget_;
                SetCapture(hwnd);
                StartAnim();
                return 0;
            }
            if (hit.kind != HitKind::None) {
                pressed_ = hit;
                SetCapture(hwnd);
                StartAnim();
                return 0;
            }
            if (y >= layout_.panel.top &&
                y <= layout_.panel.top + ui::kHeaderH &&
                x >= layout_.panel.left && x <= layout_.panel.right &&
                !confirmClose_) {
                dragging_ = true;
                SetCapture(hwnd);
                GetCursorPos(&dragCursorStart_);
                GetWindowRect(hwnd, &dragWindowStart_);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (scrollDragging_) {
                float y = GET_Y_LPARAM(lp) / scale_;
                float trackH = layout_.contentClip.bottom - layout_.contentClip.top;
                float thumbH = ScrollThumbHeight();
                float travel = std::max(1.0f, trackH - thumbH);
                float ratio = layout_.scrollMax / travel;
                scrollTarget_ = scrollDragStartTarget_ +
                                (y - scrollDragStartY_) * ratio;
                scrollTarget_ = std::min(std::max(scrollTarget_, 0.0f),
                                         layout_.scrollMax);
                StartAnim();
                return 0;
            }
            if (dragging_) {
                POINT pt = {};
                GetCursorPos(&pt);
                int newX = dragWindowStart_.left + (pt.x - dragCursorStart_.x);
                int newY = dragWindowStart_.top + (pt.y - dragCursorStart_.y);
                SetWindowPos(hwnd, nullptr, newX, newY, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (!trackingLeave_) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                trackingLeave_ = true;
            }
            // 无设备时，鼠标活动会重新起算自动隐藏倒计时
            if (emptyHideArmed_ && devices_.empty()) ArmEmptyHideTimer();
            float x = GET_X_LPARAM(lp) / scale_;
            float y = GET_Y_LPARAM(lp) / scale_;
            bool overBar =
                layout_.scrolling && InRect(layout_.scrollBar, x, y);
            if (overBar != scrollHover_) {
                scrollHover_ = overBar;
                StartAnim();
            }
            SetHover(HitTest(x, y));
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            scrollHover_ = false;
            SetHover(HitResult{});
            StartAnim();
            return 0;

        case WM_LBUTTONUP: {
            float x = GET_X_LPARAM(lp) / scale_;
            float y = GET_Y_LPARAM(lp) / scale_;
            if (scrollDragging_) {
                scrollDragging_ = false;
                pressed_ = HitResult{};
                if (GetCapture() == hwnd) ReleaseCapture();
                StartAnim();
                return 0;
            }
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                CheckDropPosition();
                return 0;
            }
            HitResult hit = HitTest(x, y);
            if (hit.kind == pressed_.kind && hit.card == pressed_.card &&
                hit.part == pressed_.part) {
                ActivateHit(hit);
            }
            pressed_ = HitResult{};
            if (GetCapture() == hwnd) ReleaseCapture();
            StartAnim();
            return 0;
        }

        case WM_CAPTURECHANGED:
            dragging_ = false;
            scrollDragging_ = false;
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt = {};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                HitResult hit =
                    HitTest(pt.x / scale_, pt.y / scale_);
                SetCursor(LoadCursorW(nullptr, hit.kind != HitKind::None ? IDC_HAND
                                                                        : IDC_ARROW));
                return TRUE;
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            if (busyDialog_) {
                float delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
                busyScrollTarget_ -= delta * 40.0f;
                busyScrollTarget_ = std::min(std::max(busyScrollTarget_, 0.0f),
                                             layout_.busyScrollMax);
                StartAnim();
                return 0;
            }
            if (layout_.scrolling) {
                float delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
                scrollTarget_ -= delta * 56.0f;
                scrollTarget_ = std::min(std::max(scrollTarget_, 0.0f),
                                         layout_.scrollMax);
                StartAnim();
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == ui::kTimerAnim) {
                if (!AnimTick()) {
                    KillTimer(hwnd, ui::kTimerAnim);
                    animating_ = false;
                }
                return 0;
            }
            if (wp == ui::kTimerRefresh) {
                KillTimer(hwnd, ui::kTimerRefresh);
                RefreshDevices();
                return 0;
            }
            if (wp == ui::kTimerPoll) {
                RefreshDevices();
                return 0;
            }
            if (wp == ui::kTimerToast) {
                KillTimer(hwnd, ui::kTimerToast);
                toastTarget_ = 0.0f;
                StartAnim();
                return 0;
            }
            if (wp == ui::kTimerEmptyHide) {
                KillTimer(hwnd, ui::kTimerEmptyHide);
                emptyHideArmed_ = false;
                if (devices_.empty() && !inTray_ && !collapsed_) HideToTray();
                return 0;
            }
            if (wp == ui::kTimerBusyScan) {
                // 扫描超时兜底：不再显示「正在查找」，允许用户重试
                KillTimer(hwnd, ui::kTimerBusyScan);
                if (busyScanning_) {
                    busyScanning_ = false;
                    if (busyDialog_) {
                        RecalcLayout();
                        PaintNow();
                    }
                }
                return 0;
            }
            break;

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE ||
                wp == DBT_DEVNODES_CHANGED) {
                SetTimer(hwnd, ui::kTimerRefresh, 280, nullptr);
                return TRUE;
            }
            break;

        case WM_APP_SHELL_NOTIFY:
            // 可用空间变化 / 介质增删：延迟合并刷新
            SetTimer(hwnd, ui::kTimerRefresh, 280, nullptr);
            return 0;

        case WM_APP_DEVICES_READY:
            OnDevicesReady();
            return 0;

        case WM_APP_SHOW_MAIN:
            ShowFromTray();
            return 0;

        case WM_MOVE:
            RecordWindowPosition();
            break;

        case WM_EXITSIZEMOVE:
            CheckDropPosition();
            break;

        case WM_DISPLAYCHANGE:
            ComputeTargetPanelHeight();
            RecalcLayout();
            StartAnim();
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            UpdateTheme();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp) ? static_cast<float>(HIWORD(wp)) : SystemDpi();
            scale_ = dpi_ / 96.0f;
            RECT* suggested = reinterpret_cast<RECT*>(lp);
            ComputeTargetPanelHeight();
            RecalcLayout();
            ApplyWindowSize();
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            PaintNow();
            return 0;
        }

        case WM_APP_TRAY:
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
                ShowFromTray();
            } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;

        case WM_APP_EJECT_DONE: {
            std::wstring message;
            std::wstring title;
            std::vector<EjectVolumeTarget> volumes;
            unsigned int diskNumber = 0;
            bool ok = false;
            bool occupied = false;
            {
                std::lock_guard<std::mutex> lock(ejectMutex_);
                if (!ejectResultPending_) return 0;
                message = ejectResultMessage_;
                title = ejectResultTitle_;
                volumes = std::move(ejectResultVolumes_);
                diskNumber = ejectResultDisk_;
                ok = ejectResultOk_;
                occupied = ejectResultOccupied_;
                ejectResultPending_ = false;
                ejectResultOccupied_ = false;
            }
            ejectBusy_ = false;
            for (auto& device : devices_) {
                if (device.diskNumber == diskNumber) device.ejecting = false;
            }
            RefreshDevices();
            if (occupied) {
                // 被占用：先立即弹出占用界面，占用进程在后台扫描后补入列表
                ShowBusyDialog(diskNumber, title, std::move(volumes), {});
                StartOccupantScan();
                return 0;
            }
            if (!ok) {
                // 设备可能已经弹出、只是磁盘节点还没消失：等枚举刷新后再确认，
                // 避免「明明已弹出却提示未能弹出」
                ejectVerifyPending_ = true;
                ejectVerifyDisk_ = diskNumber;
                ejectVerifyTitle_ = title;
                ejectVerifyMessage_ = message;
                return 0;
            }
            ShowToast(message, ok);
            if (ok) SetTimer(hwnd_, ui::kTimerRefresh, 500, nullptr);
            return 0;
        }

        case WM_APP_OCCUPANTS_READY: {
            std::vector<VolumeOccupant> list;
            {
                std::lock_guard<std::mutex> lock(occupantMutex_);
                if (!occupantScanReady_) return 0;
                list = std::move(occupantScanResult_);
                occupantScanReady_ = false;
            }
            busyScanning_ = false;
            if (hwnd_) KillTimer(hwnd_, ui::kTimerBusyScan);
            if (busyDialog_) {
                FreeOccupants(&busyOccupants_);
                busyOccupants_ = std::move(list);
                busyScroll_ = 0.0f;
                busyScrollTarget_ = 0.0f;
                RecalcLayout();
                PaintNow();
            } else {
                FreeOccupants(&list);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            DestroyEdgeBar();
            FreeOccupants(&busyOccupants_);
            {
                std::lock_guard<std::mutex> lock(occupantMutex_);
                FreeOccupants(&occupantScanResult_);
            }
            if (refreshState_) refreshState_->cancelled.store(true);
            if (shellNotifyId_ != 0) {
                SHChangeNotifyDeregister(shellNotifyId_);
                shellNotifyId_ = 0;
            }
            SaveSettings(settings_);
            if (ejectThread_.joinable()) ejectThread_.join();
            if (iconSmall_) DestroyIcon(iconSmall_);
            if (iconLarge_) DestroyIcon(iconLarge_);
            if (iconEdge_ && iconEdge_ != iconLarge_) DestroyIcon(iconEdge_);
            iconSmall_ = iconLarge_ = iconEdge_ = nullptr;
            ReleaseFonts(&fonts_);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::RefreshDevices() {
    // 枚举要逐个访问卷并查询 PnP 名称，机械硬盘刚上电时会阻塞数秒，
    // 放在后台线程里做，避免卡死界面；完成后投递消息回主线程。
    if (!hwnd_ || !refreshState_) return;
    if (refreshState_->running.load()) {
        refreshQueued_ = true;
        return;
    }
    refreshState_->running.store(true);
    refreshQueued_ = false;
    auto state = refreshState_;
    state->hwnd = hwnd_;
    std::thread([state]() {
        std::vector<DeviceInfo> fresh;
        if (!state->cancelled.load()) fresh = EnumerateRemovableDevices();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->devices = std::move(fresh);
            state->ready = true;
        }
        state->running.store(false);
        if (!state->cancelled.load() && state->hwnd) {
            PostMessageW(state->hwnd, WM_APP_DEVICES_READY, 0, 0);
        }
    }).detach();
}

void App::OnDevicesReady() {
    if (!refreshState_) return;
    std::vector<DeviceInfo> fresh;
    {
        std::lock_guard<std::mutex> lock(refreshState_->mutex);
        if (!refreshState_->ready) return;
        fresh = std::move(refreshState_->devices);
        refreshState_->devices.clear();
        refreshState_->ready = false;
    }
    ApplyDevices(std::move(fresh));
    if (ejectVerifyPending_) {
        // 这份列表可能是弹出完成前就已开始的枚举，结果仍是旧数据；
        // 若还有排队中的刷新，等它的结果再判定
        bool staleList = refreshQueued_;
        refreshQueued_ = false;
        if (staleList) {
            RefreshDevices();
        } else {
            ejectVerifyPending_ = false;
            bool present = false;
            for (const auto& device : devices_) {
                if (device.diskNumber == ejectVerifyDisk_) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                std::wstring name = ejectVerifyTitle_.empty()
                                        ? L"设备"
                                        : L"「" + ejectVerifyTitle_ + L"」";
                ShowToast(L"已安全弹出" + name + L"，现在可以拔下设备了。", true);
                SetTimer(hwnd_, ui::kTimerRefresh, 500, nullptr);
            } else {
                ShowToast(ejectVerifyMessage_, false);
            }
        }
    }
    if (refreshQueued_) {
        refreshQueued_ = false;
        RefreshDevices();
    }
}

void App::ArmEmptyHideTimer() {
    if (!hwnd_) return;
    if (settings_.emptyHideSec <= 0 || inTray_ || collapsed_) {
        KillTimer(hwnd_, ui::kTimerEmptyHide);
        emptyHideArmed_ = false;
        return;
    }
    SetTimer(hwnd_, ui::kTimerEmptyHide,
             static_cast<UINT>(settings_.emptyHideSec) * 1000, nullptr);
    emptyHideArmed_ = true;
}

void App::ApplyDevices(std::vector<DeviceInfo> fresh) {
    bool wasEmpty = devices_.empty();

    // 按物理磁盘号保留弹出状态与 hover 动画状态
    for (auto& next : fresh) {
        for (const auto& old : devices_) {
            if (old.diskNumber != next.diskNumber) continue;
            next.ejecting = old.ejecting;
            next.hoverEject = old.hoverEject;
            for (auto& part : next.parts) {
                for (const auto& oldPart : old.parts) {
                    if (part.letter != 0 && part.letter == oldPart.letter) {
                        part.hoverOpen = oldPart.hoverOpen;
                    }
                }
            }
        }
    }

    bool changed = fresh.size() != devices_.size();
    if (!changed) {
        for (size_t i = 0; i < fresh.size(); ++i) {
            const auto& a = fresh[i];
            const auto& b = devices_[i];
            if (a.diskNumber != b.diskNumber || a.title != b.title ||
                a.parts.size() != b.parts.size()) {
                changed = true;
                break;
            }
            for (size_t j = 0; j < a.parts.size(); ++j) {
                const auto& pa = a.parts[j];
                const auto& pb = b.parts[j];
                if (pa.letter != pb.letter || pa.total != pb.total ||
                    pa.freeBytes != pb.freeBytes || pa.name != pb.name ||
                    pa.fs != pb.fs || pa.openable != pb.openable) {
                    changed = true;
                    break;
                }
            }
            if (changed) break;
        }
    }

    devices_ = std::move(fresh);
    hovered_ = HitResult{};
    hoverCloseTarget_ = 0.0f;

    ComputeTargetPanelHeight();
    RecalcLayout();
    if (changed) {
        StartAnim();
    } else {
        RequestPaint();
    }

    if (!hwnd_) return;
    bool nowEmpty = devices_.empty();
    if (nowEmpty) {
        // 未插入设备：只在本轮空状态开始时起算隐藏倒计时，后续轮询不重置
        if (!emptyHideArmed_) ArmEmptyHideTimer();
    } else {
        KillTimer(hwnd_, ui::kTimerEmptyHide);
        emptyHideArmed_ = false;
        if (wasEmpty) {
            // 插入设备：仅在窗口已隐藏到托盘时唤回；靠边收起的状态需用户点击恢复
            if (inTray_) ShowFromTray();
        }
    }
}

void App::ComputeTargetPanelHeight() {
    float contentH;
    if (devices_.empty()) {
        contentH = ui::kEmptyH;
    } else {
        contentH = 0.0f;
        for (const auto& device : devices_) contentH += CardHeight(device);
        contentH += static_cast<float>(devices_.size() - 1) * ui::kCardGap;
    }

    float wanted = ui::kHeaderH + ui::kPadTop + contentH + ui::kPadBottom;

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    float screenH = (workArea.bottom - workArea.top) / scale_;
    float available = screenH - ui::kShadowPad * 2.0f - 40.0f;
    // 主界面高度上限：屏幕可用高度的百分比（超出部分改为滚动）
    float percent = static_cast<float>(settings_.mainHeightPercent) / 100.0f;
    float limit = screenH * percent - ui::kShadowPad * 2.0f;
    available = std::min(available, limit);
    targetPanelH_ = std::min(wanted, std::max(available, 120.0f));
}

void App::RecalcLayout() {
    UiLayout out;
    out.windowW = ui::kPanelW + ui::kShadowPad * 2.0f;
    out.windowH = panelH_ + ui::kShadowPad * 2.0f;
    out.panel = {ui::kShadowPad, ui::kShadowPad, ui::kShadowPad + ui::kPanelW,
                 ui::kShadowPad + panelH_};

    const float buttonSize = 26.0f;
    float centerY = out.panel.top + ui::kHeaderH * 0.5f;
    out.closeBtn = {out.panel.right - 14.0f - buttonSize, centerY - buttonSize * 0.5f,
                    out.panel.right - 14.0f, centerY + buttonSize * 0.5f};

    out.contentTop = out.panel.top + ui::kHeaderH + ui::kPadTop;
    float contentBottom = out.panel.bottom - ui::kPadBottom;
    if (contentBottom < out.contentTop) contentBottom = out.contentTop;
    out.contentClip = {out.panel.left + ui::kPadX, out.contentTop,
                       out.panel.right - ui::kPadX, contentBottom};

    float viewportH = out.contentClip.bottom - out.contentClip.top;
    float contentH;
    if (devices_.empty()) {
        contentH = ui::kEmptyH;
    } else {
        contentH = 0.0f;
        for (const auto& device : devices_) contentH += CardHeight(device);
        contentH += static_cast<float>(devices_.size() - 1) * ui::kCardGap;
    }
    out.scrollMax = std::max(0.0f, contentH - viewportH);
    out.scrolling = out.scrollMax > 0.5f;
    if (out.scrolling) {
        // 滚动条可交互区域（比滑块宽一些，便于命中）
        out.scrollBar = {out.panel.right - 12.0f, out.contentClip.top,
                         out.panel.right - 2.0f, out.contentClip.bottom};
    }
    scrollY_ = std::min(std::max(scrollY_, 0.0f), out.scrollMax);
    scrollTarget_ = std::min(std::max(scrollTarget_, 0.0f), out.scrollMax);

    const float cardWidth = out.contentClip.right - out.contentClip.left;
    float top = out.contentTop - scrollY_;
    for (size_t i = 0; i < devices_.size(); ++i) {
        const DeviceInfo& device = devices_[i];
        const float height = CardHeight(device);
        CardLayout card;
        card.card = {out.contentClip.left, top, out.contentClip.left + cardWidth,
                     top + height};
        float right = card.card.right - 14.0f;
        card.ejectBtn = {right - ui::kEjectW, card.card.top + 8.0f, right,
                         card.card.top + ui::kCardHeaderH - 8.0f};
        for (size_t j = 0; j < device.parts.size(); ++j) {
            float rowTop = card.card.top + ui::kCardHeaderH +
                           ui::kPartH * static_cast<float>(j);
            PartitionLayout part;
            part.row = {card.card.left, rowTop, card.card.right, rowTop + ui::kPartH};
            part.pill = {card.card.left + 14.0f, rowTop + 8.0f,
                         card.card.left + 54.0f, rowTop + 28.0f};
            part.openBtn = {right - ui::kOpenW, rowTop + 12.0f, right,
                            rowTop + 12.0f + ui::kBtnH};
            card.parts.push_back(part);
        }
        out.cards.push_back(std::move(card));
        top += height + ui::kCardGap;
    }

    // 退出确认弹层（居中）
    out.confirmVisible = confirmClose_;
    if (confirmClose_) {
        const float dw = 300.0f;
        const float dh = 156.0f;
        float dx = out.panel.left + (out.panel.right - out.panel.left - dw) * 0.5f;
        float dy = out.panel.top + (out.panel.bottom - out.panel.top - dh) * 0.5f;
        out.dlgCard = {dx, dy, dx + dw, dy + dh};
        out.dlgCheck = {dx + 18.0f, dy + dh - 50.0f, dx + 34.0f, dy + dh - 34.0f};
        const float bw = 76.0f;
        const float bh = 28.0f;
        out.dlgOk = {dx + dw - 16.0f - bw, dy + dh - 14.0f - bh, dx + dw - 16.0f,
                     dy + dh - 14.0f};
        out.dlgCancel = {out.dlgOk.left - 10.0f - bw, out.dlgOk.top,
                         out.dlgOk.left - 10.0f, out.dlgOk.bottom};
    }

    // 设备被占用弹层（居中，含进程列表与滚动条）
    out.busyVisible = busyDialog_;
    if (busyDialog_) {
        const float dw = 332.0f;
        const float dh = 236.0f;
        float dx = out.panel.left + (out.panel.right - out.panel.left - dw) * 0.5f;
        float dy = out.panel.top + (out.panel.bottom - out.panel.top - dh) * 0.5f;
        out.busyCard = {dx, dy, dx + dw, dy + dh};
        out.busyTitle = {dx + 18.0f, dy + 18.0f, dx + dw - 18.0f, dy + 62.0f};
        out.busyList = {dx + 18.0f, dy + 66.0f, dx + dw - 18.0f, dy + dh - 52.0f};
        const float bw = 92.0f;
        const float bh = 28.0f;
        out.busyRelease = {dx + dw - 16.0f - bw, dy + dh - 14.0f - bh,
                           dx + dw - 16.0f, dy + dh - 14.0f};
        out.busyCancel = {out.busyRelease.left - 10.0f - 72.0f,
                          out.busyRelease.top, out.busyRelease.left - 10.0f,
                          out.busyRelease.bottom};
        const float rowH = 34.0f;
        float viewport = out.busyList.bottom - out.busyList.top;
        float content = static_cast<float>(busyOccupants_.size()) * rowH + 8.0f;
        out.busyScrollMax = std::max(0.0f, content - viewport);
        busyScroll_ = std::min(std::max(busyScroll_, 0.0f), out.busyScrollMax);
        busyScrollTarget_ =
            std::min(std::max(busyScrollTarget_, 0.0f), out.busyScrollMax);
    }

    layout_ = std::move(out);
}

void App::ApplyTopMost() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, settings_.topMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::ApplyRefreshTimer() {
    if (!hwnd_) return;
    KillTimer(hwnd_, ui::kTimerPoll);
    if (settings_.refreshSec > 0) {
        SetTimer(hwnd_, ui::kTimerPoll,
                 static_cast<UINT>(settings_.refreshSec) * 1000, nullptr);
    }
}

// 按设置计算窗口初始位置：相对某个屏幕角落，或上次的坐标（至少露出 64px）
POINT App::DefaultWindowOrigin(int widthPx, int heightPx) const {
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int margin = static_cast<int>(std::lround(18.0f * scale_));

    if (settings_.posMode == 1 && settings_.hasLastPos) {
        POINT pt = {settings_.lastX, settings_.lastY};
        if (pt.x > workArea.right - 64) pt.x = workArea.right - 64;
        if (pt.y > workArea.bottom - 64) pt.y = workArea.bottom - 64;
        if (pt.x + widthPx < workArea.left + 64) pt.x = workArea.left + 64 - widthPx;
        if (pt.y + heightPx < workArea.top + 64) pt.y = workArea.top + 64 - heightPx;
        return pt;
    }

    POINT pt = {};
    switch (settings_.corner) {
        case 0: // 左上
            pt = {workArea.left + margin, workArea.top + margin};
            break;
        case 1: // 左下
            pt = {workArea.left + margin, workArea.bottom - heightPx - margin};
            break;
        case 2: // 右上
            pt = {workArea.right - widthPx - margin, workArea.top + margin};
            break;
        default: // 右下
            pt = {workArea.right - widthPx - margin,
                  workArea.bottom - heightPx - margin};
            break;
    }
    return pt;
}

void App::RecordWindowPosition() {
    if (settings_.posMode != 1 || !hwnd_) return;
    RECT rc = {};
    GetWindowRect(hwnd_, &rc);
    settings_.lastX = rc.left;
    settings_.lastY = rc.top;
    settings_.hasLastPos = true;
}

void App::ApplyWindowSize() {
    int widthPx = static_cast<int>(std::lround(layout_.windowW * scale_));
    int heightPx = static_cast<int>(std::lround(layout_.windowH * scale_));
    if (widthPx <= 0 || heightPx <= 0) return;

    RECT current = {};
    GetWindowRect(hwnd_, &current);
    int currentW = current.right - current.left;
    int currentH = current.bottom - current.top;
    if (currentW != widthPx || currentH != heightPx) {
        RECT workArea = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        int left = current.left;
        int top = current.top;
        int minX = workArea.left + 8;
        int minY = workArea.top + 8;
        if (left + widthPx > workArea.right - 8) left = workArea.right - 8 - widthPx;
        if (left < minX) left = minX;
        if (top + heightPx > workArea.bottom - 8) top = workArea.bottom - 8 - heightPx;
        if (top < minY) top = minY;
        SetWindowPos(hwnd_, nullptr, left, top, widthPx, heightPx,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    canvas_.Resize(widthPx, heightPx);
}

void App::RequestPaint() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::PaintNow() {
    if (!canvas_.Valid() || !hwnd_) return;

    canvas_.Begin(dpi_);
    PaintApp(*this, canvas_, layout_);
    canvas_.End();

    float alpha = std::min(1.0f, std::max(0.0f, windowAlpha_));
    canvas_.Present(hwnd_, static_cast<unsigned char>(std::lround(alpha * 255.0f)));
}

void App::StartAnim() {
    if (!hwnd_ || animating_) return;
    animating_ = true;
    SetTimer(hwnd_, ui::kTimerAnim, 16, nullptr);
}

bool App::AnimTick() {
    bool more = false;
    auto ease = [&more](float& value, float target, float rate, float epsilon) {
        float diff = target - value;
        if (std::fabs(diff) <= epsilon) {
            if (value != target) {
                value = target;
                more = true;
            }
            return;
        }
        value += diff * rate;
        more = true;
    };

    ease(panelH_, targetPanelH_, 0.20f, 0.35f);
    ease(scrollY_, scrollTarget_, 0.22f, 0.35f);
    ease(busyScroll_, busyScrollTarget_, 0.22f, 0.35f);
    ease(windowAlpha_, 1.0f, 0.10f, 0.01f);
    ease(hoverClose_, hoverCloseTarget_, 0.26f, 0.008f);
    ease(toastOpacity_, toastTarget_, 0.22f, 0.01f);
    ease(scrollGrow_, (scrollHover_ || scrollDragging_) ? 1.0f : 0.0f, 0.30f,
         0.008f);

    // 边缘悬浮条恢复时的滑入动画（物理像素，四个方向都可能为负/正）
    if (std::fabs(slideOffset_) > 0.5f || std::fabs(slideOffsetY_) > 0.5f) {
        ease(slideOffset_, 0.0f, 0.28f, 1.0f);
        ease(slideOffsetY_, 0.0f, 0.28f, 1.0f);
        if (hwnd_) {
            SetWindowPos(hwnd_, nullptr,
                         restoreX_ + static_cast<int>(slideOffset_),
                         restoreY_ + static_cast<int>(slideOffsetY_), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    for (size_t i = 0; i < devices_.size(); ++i) {
        DeviceInfo& device = devices_[i];
        ease(device.hoverEject,
             (hovered_.kind == HitKind::Eject && hovered_.card == static_cast<int>(i))
                 ? 1.0f
                 : 0.0f,
             0.26f, 0.008f);
        for (size_t j = 0; j < device.parts.size(); ++j) {
            PartitionInfo& part = device.parts[j];
            ease(part.hoverOpen,
                 (hovered_.kind == HitKind::Open &&
                  hovered_.card == static_cast<int>(i) &&
                  hovered_.part == static_cast<int>(j))
                     ? 1.0f
                     : 0.0f,
                 0.26f, 0.008f);
        }
    }

    if (toastOpacity_ <= 0.02f && toastTarget_ == 0.0f) toastVisible_ = false;

    if (!more) return false;

    RecalcLayout();
    ApplyWindowSize();
    PaintNow();
    return more;
}

void App::SetHover(HitResult hit) {
    if (hit.kind == hovered_.kind && hit.card == hovered_.card &&
        hit.part == hovered_.part) {
        return;
    }
    hovered_ = hit;
    hoverCloseTarget_ = hit.kind == HitKind::Close ? 1.0f : 0.0f;
    StartAnim();
}

float App::ScrollThumbHeight() const {
    float trackH = layout_.contentClip.bottom - layout_.contentClip.top;
    if (trackH <= 0.0f) return 0.0f;
    float content = trackH + layout_.scrollMax;
    if (content <= 0.0f) return trackH;
    return std::max(28.0f, trackH * trackH / content);
}

D2D1_RECT_F App::ScrollThumbRect() const {
    float trackH = layout_.contentClip.bottom - layout_.contentClip.top;
    float thumbH = ScrollThumbHeight();
    float progress =
        layout_.scrollMax > 0.0f ? scrollY_ / layout_.scrollMax : 0.0f;
    progress = std::min(std::max(progress, 0.0f), 1.0f);
    float top = layout_.contentClip.top + (trackH - thumbH) * progress;
    return {layout_.panel.right - 12.0f, top, layout_.panel.right - 2.0f,
            top + thumbH};
}

HitResult App::HitTest(float x, float y) const {
    // 占用弹层显示时，只有弹层内可交互
    if (busyDialog_) {
        if (InRect(layout_.busyRelease, x, y)) return {HitKind::BusyRelease, -1, -1};
        if (InRect(layout_.busyCancel, x, y)) return {HitKind::BusyCancel, -1, -1};
        return HitResult{};
    }

    // 确认弹层显示时，只有弹层内可交互
    if (confirmClose_) {
        if (InRect(layout_.dlgOk, x, y)) return {HitKind::ConfirmOk, -1, -1};
        if (InRect(layout_.dlgCancel, x, y)) return {HitKind::ConfirmCancel, -1, -1};
        D2D1_RECT_F checkHit = layout_.dlgCheck;
        checkHit.right += 90.0f; // 含「不再提示」文字
        checkHit.top -= 4.0f;
        checkHit.bottom += 4.0f;
        if (InRect(checkHit, x, y)) return {HitKind::ConfirmCheck, -1, -1};
        return HitResult{};
    }

    if (InRect(layout_.closeBtn, x, y)) return {HitKind::Close, -1, -1};

    if (layout_.scrolling && InRect(layout_.scrollBar, x, y)) {
        return {HitKind::ScrollBar, -1, -1};
    }

    if (!InRect(layout_.contentClip, x, y)) return HitResult{};

    for (size_t i = 0; i < layout_.cards.size(); ++i) {
        const CardLayout& card = layout_.cards[i];
        if (i < devices_.size() && devices_[i].ejecting) {
            // 弹出进行中：只保留关闭可点
            continue;
        }
        if (InRect(card.ejectBtn, x, y)) {
            return {HitKind::Eject, static_cast<int>(i), -1};
        }
        for (size_t j = 0; j < card.parts.size(); ++j) {
            if (j < devices_[i].parts.size() && !devices_[i].parts[j].openable) continue;
            if (InRect(card.parts[j].openBtn, x, y)) {
                return {HitKind::Open, static_cast<int>(i), static_cast<int>(j)};
            }
        }
    }
    return HitResult{};
}

void App::ActivateHit(HitResult hit) {
    switch (hit.kind) {
        case HitKind::Close:
            if (settings_.closeAction == 1) {
                HideToTray();
            } else if (settings_.skipCloseConfirm) {
                DestroyWindow(hwnd_);
            } else {
                confirmClose_ = true;
                confirmDontAsk_ = false;
                RecalcLayout();
                PaintNow();
            }
            break;
        case HitKind::ConfirmOk:
            if (confirmDontAsk_) {
                settings_.skipCloseConfirm = true;
                SaveSettings(settings_);
            }
            DestroyWindow(hwnd_);
            break;
        case HitKind::ConfirmCancel:
            confirmClose_ = false;
            RecalcLayout();
            PaintNow();
            break;
        case HitKind::ConfirmCheck:
            confirmDontAsk_ = !confirmDontAsk_;
            PaintNow();
            break;
        case HitKind::Open:
            if (hit.card >= 0 && hit.card < static_cast<int>(devices_.size())) {
                const DeviceInfo& device = devices_[hit.card];
                if (hit.part >= 0 && hit.part < static_cast<int>(device.parts.size())) {
                    const PartitionInfo& part = device.parts[hit.part];
                    if (part.openable && !part.root.empty()) {
                        ShellExecuteW(nullptr, L"open", part.root.c_str(), nullptr,
                                      nullptr, SW_SHOWNORMAL);
                    }
                }
            }
            break;
        case HitKind::Eject:
            StartEject(hit.card);
            break;
        case HitKind::BusyCancel:
            CloseBusyDialog();
            break;
        case HitKind::BusyRelease:
            ReleaseOccupantsAndEject();
            break;
        default:
            break;
    }
}

void App::StartEject(int cardIndex) {
    if (cardIndex < 0 || cardIndex >= static_cast<int>(devices_.size())) return;
    const DeviceInfo& device = devices_[cardIndex];
    if (device.ejecting || ejectBusy_) return;

    if (ejectThread_.joinable()) ejectThread_.join();

    unsigned int diskNumber = device.diskNumber;
    std::wstring title = device.title;
    std::vector<EjectVolumeTarget> volumes;
    for (const auto& part : device.parts) {
        if (!part.volumePath.empty()) {
            volumes.push_back(EjectVolumeTarget{part.volumePath, part.letter});
        }
    }

    devices_[cardIndex].ejecting = true;
    ejectBusy_ = true;

    StartAnim();
    PaintNow();

    ejectThread_ = std::thread([this, diskNumber, title, volumes]() {
        EjectOutcome outcome = EjectDisk(diskNumber, title, volumes);
        {
            std::lock_guard<std::mutex> lock(ejectMutex_);
            ejectResultPending_ = true;
            ejectResultOk_ = outcome.status == EjectStatus::Ok;
            ejectResultOccupied_ = outcome.status == EjectStatus::Busy;
            ejectResultDisk_ = diskNumber;
            ejectResultMessage_ = outcome.message;
            ejectResultTitle_ = title;
            ejectResultVolumes_ = volumes;
        }
        PostMessageW(hwnd_, WM_APP_EJECT_DONE, 0, 0);
    });
}

void App::ShowToast(const std::wstring& text, bool ok) {
    toastText_ = text;
    toastOk_ = ok;
    toastVisible_ = true;
    toastTarget_ = 1.0f;
    if (toastOpacity_ < 0.05f) toastOpacity_ = 0.05f;
    SetTimer(hwnd_, ui::kTimerToast, 4200, nullptr);
    StartAnim();
}

void App::ShowBusyDialog(unsigned int disk, const std::wstring& title,
                         std::vector<EjectVolumeTarget> volumes,
                         std::vector<VolumeOccupant> occupants) {
    if (busyDialog_) FreeOccupants(&busyOccupants_);
    busyDialog_ = true;
    busyTitle_ = title;
    busyDisk_ = disk;
    busyVolumes_ = std::move(volumes);
    busyOccupants_ = std::move(occupants);
    busyScanning_ = false;
    busyScroll_ = 0.0f;
    busyScrollTarget_ = 0.0f;
    RecalcLayout();
    PaintNow();
}

void App::StartOccupantScan() {
    if (busyScanning_) return;
    busyScanning_ = true;
    std::vector<wchar_t> letters;
    for (const auto& volume : busyVolumes_) {
        if (volume.letter != 0) letters.push_back(volume.letter);
    }
    HWND hwnd = hwnd_;
    std::thread([this, letters, hwnd]() {
        std::vector<VolumeOccupant> list = EnumerateVolumeOccupants(letters);
        {
            std::lock_guard<std::mutex> lock(occupantMutex_);
            FreeOccupants(&occupantScanResult_);
            occupantScanResult_ = std::move(list);
            occupantScanReady_ = true;
        }
        PostMessageW(hwnd, WM_APP_OCCUPANTS_READY, 0, 0);
    }).detach();
    // 超时兜底：即使扫描异常卡住，界面也不会一直停在「正在查找」
    if (hwnd_) SetTimer(hwnd_, ui::kTimerBusyScan, 12000, nullptr);
    PaintNow();
}

void App::CloseBusyDialog() {
    if (!busyDialog_) return;
    FreeOccupants(&busyOccupants_);
    busyDialog_ = false;
    busyScanning_ = false;
    if (hwnd_) KillTimer(hwnd_, ui::kTimerBusyScan);
    busyScroll_ = 0.0f;
    busyScrollTarget_ = 0.0f;
    RecalcLayout();
    PaintNow();
}

void App::ReleaseOccupantsAndEject() {
    if (!busyDialog_) return;
    // 扫描尚未结束：先等列表出来再操作，避免「点了没反应」
    if (busyScanning_) {
        ShowToast(L"正在查找占用进程，请稍候重试。", false);
        return;
    }
    std::vector<DWORD> pids;
    for (const auto& occupant : busyOccupants_) pids.push_back(occupant.pid);

    unsigned int disk = busyDisk_;
    CloseBusyDialog();

    int closed = TerminateOccupants(pids);
    // 给系统一点时间释放被结束进程的句柄
    if (closed > 0) Sleep(300);

    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].diskNumber == disk) {
            StartEject(static_cast<int>(i));
            return;
        }
    }
    // 设备已不在列表里，说明上一次弹出其实已经成功
    ShowToast(L"已安全弹出设备，现在可以拔下设备了。", true);
}

void App::UpdateTheme() {
    bool dark = QuerySystemDarkMode();
    if (dark == palette_.dark) return;
    palette_ = MakePalette(dark);
    PaintNow();
}

void App::AddTrayIcon() {
    if (!hwnd_) return;
    trayData_ = {};
    trayData_.cbSize = sizeof(trayData_);
    trayData_.hWnd = hwnd_;
    trayData_.uID = 1;
    trayData_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayData_.uCallbackMessage = WM_APP_TRAY;
    trayData_.hIcon = iconSmall_;
    wcscpy(trayData_.szTip, L"U 盘助手");
    Shell_NotifyIconW(NIM_ADD, &trayData_);
}

void App::RemoveTrayIcon() {
    if (!hwnd_) return;
    trayData_.hWnd = hwnd_;
    trayData_.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &trayData_);
}

void App::ShowTrayMenu() {
    POINT pt = {};
    GetCursorPos(&pt);
    std::vector<std::wstring> items = {L"显示窗口", L"设置…", L"退出"};
    int choice = ShowTrayMenuPopup(hinst_, pt, items, 2);
    if (choice == 0) {
        ShowFromTray();
    } else if (choice == 1) {
        ShowSettings();
    } else if (choice == 2) {
        DestroyWindow(hwnd_);
    }
}

void App::HideToTray() {
    collapsed_ = false;
    DestroyEdgeBar();
    if (hwnd_) KillTimer(hwnd_, ui::kTimerEmptyHide);
    emptyHideArmed_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    inTray_ = true;
}

void App::ShowSettings() {
    ShowSettingsWindow(hinst_, &settings_, [this]() { ApplySettings(); });
}

void App::ApplySettings() {
    SetCapacityBase(settings_.capacityBase);
    ApplyTopMost();
    ApplyRefreshTimer();
    RefreshDevices(); // 容量进制可能已变，立即按新风格重绘
    // 自动隐藏时间可能已改，重新起算
    if (hwnd_) KillTimer(hwnd_, ui::kTimerEmptyHide);
    emptyHideArmed_ = false;
    if (devices_.empty()) ArmEmptyHideTimer();
    PaintNow();
}

void App::CheckDropPosition() {
    if (!hwnd_ || collapsed_ || inTray_) return;
    // 「靠边时隐藏到悬浮窗」关闭时，任何情况都不出现悬浮窗
    if (!settings_.edgeCollapse) return;

    RECT win = {};
    GetWindowRect(hwnd_, &win);
    // 去掉四周阴影留白，按实际窗体边界判断
    int pad = static_cast<int>(std::lround(ui::kShadowPad * scale_));
    RECT panel = {win.left + pad, win.top + pad, win.right - pad, win.bottom - pad};

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    // 任务栏所在的那条边界不参与判定
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    bool allowLeft = work.left <= 0;
    bool allowTop = work.top <= 0;
    bool allowRight = work.right >= screenW;
    bool allowBottom = work.bottom >= screenH;

    int margin = static_cast<int>(std::lround(24.0f * scale_));
    // 得分越小越贴近该边缘；被任务栏占用或不可用的边给一个大值
    int farScore = 100000;
    int leftScore = allowLeft ? panel.left - (work.left + margin) : farScore;
    int rightScore = allowRight ? (work.right - margin) - panel.right : farScore;
    int topScore = allowTop ? panel.top - (work.top + margin) : farScore;
    int bottomScore = allowBottom ? (work.bottom - margin) - panel.bottom : farScore;

    int best = std::min(std::min(leftScore, rightScore),
                        std::min(topScore, bottomScore));
    if (best > 0) return; // 没有靠边，也没有出界

    int edge = 1; // 右
    if (best == leftScore) edge = 0;
    else if (best == rightScore) edge = 1;
    else if (best == topScore) edge = 2;
    else edge = 3;
    CollapseToEdgeBar(edge);
}

void App::CollapseToEdgeBar(int edge) {
    if (collapsed_ || !hwnd_) return;
    if (!settings_.edgeCollapse) return;

    RECT win = {};
    GetWindowRect(hwnd_, &win);
    int pad = static_cast<int>(std::lround(ui::kShadowPad * scale_));
    // 实际窗体边界（不含阴影）
    RECT panel = {win.left + pad, win.top + pad, win.right - pad, win.bottom - pad};
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    // 悬浮窗取最初图标尺寸（56）的 75%；图标是正方形，因此宽高一致
    int side = static_cast<int>(std::lround(56.0f * 0.75f));
    int maxSide = std::min(work.right - work.left, work.bottom - work.top) - 16;
    if (side > maxSide) side = maxSide;
    if (side < 32) side = 32;

    auto clampX = [&](int x) {
        if (x + side > work.right) x = work.right - side;
        if (x < work.left) x = work.left;
        return x;
    };
    auto clampY = [&](int y) {
        if (y + side > work.bottom) y = work.bottom - side;
        if (y < work.top) y = work.top;
        return y;
    };

    // 贴到触发的那个边缘；左/右用窗体上边界对齐，上/下用窗体左边界对齐
    int x = work.right - side;
    int y = clampY(panel.top);
    if (edge == 0) {
        x = work.left;
        y = clampY(panel.top);
    } else if (edge == 1) {
        x = work.right - side;
        y = clampY(panel.top);
    } else if (edge == 2) {
        x = clampX(panel.left);
        y = work.top;
    } else {
        x = clampX(panel.left);
        y = work.bottom - side;
    }
    edgeSide_ = edge;

    collapsed_ = true;
    ShowWindow(hwnd_, SW_HIDE);

    if (!edgeBar_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &App::StaticEdgeBarProc;
            wc.hInstance = hinst_;
            wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
            wc.lpszClassName = L"UsbAssistantEdgeBar";
            RegisterClassExW(&wc);
            registered = true;
        }
        edgeBar_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   L"UsbAssistantEdgeBar", L"", WS_POPUP, x, y,
                                   side, side, nullptr, nullptr, hinst_, this);
    }
    if (edgeBar_) {
        // 圆角外形
        HRGN rgn = CreateRoundRectRgn(0, 0, side + 1, side + 1, 14, 14);
        SetWindowRgn(edgeBar_, rgn, TRUE);
        SetWindowPos(edgeBar_, HWND_TOPMOST, x, y, side, side, SWP_NOACTIVATE);
        ShowWindow(edgeBar_, SW_SHOWNOACTIVATE);
        InvalidateRect(edgeBar_, nullptr, TRUE);
    }
}

void App::RestoreFromEdgeBar() {
    if (!collapsed_ || !hwnd_) return;
    collapsed_ = false;
    DestroyEdgeBar();

    RECT rc = {};
    GetWindowRect(hwnd_, &rc);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    // 收起前窗口可能在屏幕外，恢复时拉回到可见区域
    int x = rc.left;
    int y = rc.top;
    if (x + w > work.right) x = work.right - w;
    if (x < work.left) x = work.left;
    if (y + h > work.bottom) y = work.bottom - h;
    if (y < work.top) y = work.top;
    restoreX_ = x;
    restoreY_ = y;
    switch (edgeSide_) {
        case 0: // 左：从屏幕左侧滑出
            slideOffset_ = -72.0f;
            slideOffsetY_ = 0.0f;
            break;
        case 2: // 上：从屏幕上方滑出
            slideOffset_ = 0.0f;
            slideOffsetY_ = -72.0f;
            break;
        case 3: // 下：从屏幕下方滑出
            slideOffset_ = 0.0f;
            slideOffsetY_ = 72.0f;
            break;
        default: // 右：从屏幕右侧滑出
            slideOffset_ = 72.0f;
            slideOffsetY_ = 0.0f;
            break;
    }
    SetWindowPos(hwnd_, nullptr,
                 restoreX_ + static_cast<int>(slideOffset_),
                 restoreY_ + static_cast<int>(slideOffsetY_), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    inTray_ = false;
    windowAlpha_ = 1.0f;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ApplyTopMost();
    PaintNow();
    StartAnim();
}

void App::DestroyEdgeBar() {
    if (edgeBar_) {
        DestroyWindow(edgeBar_);
        edgeBar_ = nullptr;
    }
}

LRESULT CALLBACK App::StaticEdgeBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc = {};
            GetClientRect(hwnd, &rc);

            bool dark = QuerySystemDarkMode();
            HBRUSH bg = CreateSolidBrush(dark ? RGB(0x2A, 0x2A, 0x2D)
                                              : RGB(0xFF, 0xFF, 0xFF));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            // 应用图标铺满整个悬浮窗，不留空白背景
            HICON icon = self->iconEdge_ ? self->iconEdge_ : self->iconLarge_;
            if (icon) {
                DrawIconEx(hdc, 0, 0, icon, rc.right - rc.left,
                           rc.bottom - rc.top, 0, nullptr, DI_NORMAL);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP:
            self->RestoreFromEdgeBar();
            return 0;
        case WM_NCDESTROY:
            self->edgeBar_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::ShowFromTray() {
    if (collapsed_) {
        RestoreFromEdgeBar();
        return;
    }
    inTray_ = false;
    windowAlpha_ = 0.0f;
    PaintNow();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ApplyTopMost();
    StartAnim();
    // 重新显示后，无设备时重新起算自动隐藏
    if (devices_.empty()) {
        KillTimer(hwnd_, ui::kTimerEmptyHide);
        emptyHideArmed_ = false;
        ArmEmptyHideTimer();
    }
}
