#include "app/application.h"

#include <d2d1helper.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/devices.h"
#include "core/eject.h"
#include "core/elevation.h"
#include "core/log.h"
#include "core/occupants.h"
#include "core/storage.h"
#include "core/text.h"
#include "core/update.h"
#include "core/version.h"
#include "ui/draw.h"

namespace usb::app {
namespace {

constexpr wchar_t kWindowClass[] = L"UsbAssistantMain";
constexpr wchar_t kWindowTitle[] = L"U 盘助手";

constexpr UINT kMessageTray = WM_APP + 1;
constexpr UINT kMessageScanDone = WM_APP + 2;
constexpr UINT kMessageEjectDone = WM_APP + 3;
constexpr UINT kMessageOccupantsDone = WM_APP + 4;
constexpr UINT kMessageHelperDone = WM_APP + 5;
constexpr UINT kMessageShellNotify = WM_APP + 6;
constexpr UINT kMessageShowMain = WM_APP + 7;
constexpr UINT kMessageUpdateDone = WM_APP + 8;

constexpr UINT_PTR kTimerAnim = 1;
constexpr UINT_PTR kTimerScan = 2;
constexpr UINT_PTR kTimerPoll = 3;
constexpr UINT_PTR kTimerEmptyHide = 4;
constexpr UINT_PTR kTimerTray = 5;
constexpr UINT_PTR kTimerToast = 6;
constexpr UINT_PTR kTimerAction = 7;
constexpr UINT_PTR kTimerTrayHealth = 8;
constexpr UINT_PTR kTimerOccupantsRefresh = 9;
constexpr UINT_PTR kTimerUpdateAuto = 10;

constexpr int kActionTimeoutEjectMs = 30000;
constexpr int kActionTimeoutScanMs = 15000;
constexpr int kActionTimeoutReleaseMs = 20000;
constexpr int kHelperProcessTimeoutMs = 60000;

struct ScanPayload {
    std::shared_ptr<const std::vector<Device>> devices;
};

struct EjectPayload {
    std::uint64_t generation = 0;
    EjectResult result;
    std::uint32_t disk_number = 0;
    std::wstring title;
    std::vector<EjectTarget> targets;
    bool from_release = false;
    int released = 0;
    int denied = 0;
    int explorer = 0;
    int skipped = 0;
};

struct OccupantsPayload {
    std::uint64_t generation = 0;
    std::shared_ptr<const std::vector<Occupant>> occupants;
    bool refresh = false;   // 占用列表的后台复检，不显示“扫描中”
    bool complete = false;  // 扫描是否完整（失败时保留现有列表）
};

struct HelperPayload {
    std::uint64_t generation = 0;
    HelperOutcome outcome;
    HelperOperation operation = HelperOperation::Eject;
    std::uint32_t disk_number = 0;
    std::wstring title;
    std::vector<wchar_t> letters;
    std::wstring volume_path;
    wchar_t letter = 0;
    std::wstring root;
};

struct UpdatePayload {
    std::uint64_t generation = 0;
    UpdateCheckResult result;
};

template <typename T>
void PostPayload(const std::shared_ptr<PostTarget>& target, UINT message,
                 std::unique_ptr<T> payload) {
    if (!target) return;
    std::lock_guard<std::mutex> lock(target->mutex);
    if (!target->hwnd) return;
    if (!PostMessageW(target->hwnd, message, 0,
                      reinterpret_cast<LPARAM>(payload.get()))) {
        return;  // unique_ptr 负责释放
    }
    payload.release();
}

template <typename T>
std::unique_ptr<T> TakePayload(LPARAM lparam) {
    return std::unique_ptr<T>(reinterpret_cast<T*>(lparam));
}

float SystemDpi() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) : 96.0f;
}

RECT WorkAreaForMonitor(HMONITOR monitor) {
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

RECT WorkAreaForWindow(HWND hwnd) {
    return WorkAreaForMonitor(
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

RECT WorkAreaForPoint(POINT point) {
    return WorkAreaForMonitor(
        MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
}

RECT MonitorAreaForWindow(HWND hwnd) {
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcMonitor;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN)};
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

void EnableDpiAwareness() {
    using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetDpiContextFn>(
            reinterpret_cast<void*>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (fn && fn(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) {
            return;  // PER_MONITOR_AWARE_V2
        }
        auto legacy = reinterpret_cast<BOOL(WINAPI*)()>(
            reinterpret_cast<void*>(
                GetProcAddress(user32, "SetProcessDPIAware")));
        if (legacy) legacy();
    }
}

bool Ease(float& value, float target, float rate, float epsilon) {
    const float delta = target - value;
    if (std::fabs(delta) <= epsilon) {
        if (value != target) {
            value = target;
            return true;
        }
        return false;
    }
    value += delta * rate;
    return true;
}

std::vector<wchar_t> LettersFromTargets(
    const std::vector<EjectTarget>& targets) {
    std::vector<wchar_t> letters;
    for (const auto& target : targets) {
        if (target.letter != 0) letters.push_back(target.letter);
    }
    return letters;
}

// 后台复检结果与当前列表是否一致：只比 PID，避免无意义的整窗重绘。
std::vector<DWORD> OccupantPids(const std::vector<Occupant>& occupants) {
    std::vector<DWORD> pids;
    pids.reserve(occupants.size());
    for (const auto& occupant : occupants) pids.push_back(occupant.pid);
    std::sort(pids.begin(), pids.end());
    return pids;
}

std::vector<EjectTarget> TargetsFromLetters(
    const std::vector<wchar_t>& letters) {
    std::vector<EjectTarget> targets;
    for (wchar_t letter : letters) {
        if (letter == 0) continue;
        wchar_t root[4] = {letter, L':', L'\\', 0};
        wchar_t volume_name[MAX_PATH] = {};
        if (!GetVolumeNameForVolumeMountPointW(root, volume_name, MAX_PATH)) {
            continue;
        }
        targets.push_back(
            EjectTarget{storage::OpenableVolumePath(volume_name), letter});
    }
    return targets;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L"";
    return path;
}

std::wstring PartitionMountLabel(const Partition& part) {
    if (part.kind == PartitionKind::Esp) return L"ESP 分区";
    if (part.kind == PartitionKind::Msr) return L"MSR 分区";
    if (!part.label.empty()) return part.label;
    return L"该分区";
}


}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

Application::Application() {
    devices_ = std::make_shared<const std::vector<Device>>();
    occupants_ = std::make_shared<const std::vector<Occupant>>();
}

Application::~Application() {
    // 退出时不等后台任务：Helper 可能还在等 UAC/子进程，EjectDevice 也可能
    // 正在等待设备消失。任务只访问 core 与 PostTarget，不访问 Application。
    scan_worker_.Stop(false);
    action_worker_.Stop(false);
    update_worker_.Stop(false);

    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    UnmountManagedVolumes(PartitionKind::Normal, true);
    settings_window_.Close();
    about_window_.Close();
    edge_bar_.Destroy();
    if (canvas_.valid()) canvas_.Release();
    ui::ReleaseFonts(&fonts_);
    if (dwrite_) {
        dwrite_->Release();
        dwrite_ = nullptr;
    }
    if (d2d_) {
        d2d_->Release();
        d2d_ = nullptr;
    }
    if (icon_small_) DestroyIcon(icon_small_);
    if (icon_large_ && icon_large_ != icon_small_) DestroyIcon(icon_large_);
    if (icon_edge_ && icon_edge_ != icon_large_ && icon_edge_ != icon_small_) {
        DestroyIcon(icon_edge_);
    }
    if (com_initialized_) CoUninitialize();
}

bool Application::Initialize(HINSTANCE instance, bool start_hidden) {
    instance_ = instance;
    start_hidden_ = start_hidden;

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(com);
    EnableDpiAwareness();
    dpi_ = SystemDpi();
    scale_ = dpi_ / 96.0f;

    settings_ = store_.Load();
    USB_LOG_INFO(L"app", L"启动：elevated=%d start_hidden=%d dpi=%.0f",
                 IsProcessElevated() ? 1 : 0, start_hidden ? 1 : 0, dpi_);
    palette_ = ui::MakePalette(ui::IsSystemDarkMode());

    const GUID d2d1_factory_iid = {
        0x06152247, 0x6f50, 0x465a,
        {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 d2d1_factory_iid, nullptr,
                                 reinterpret_cast<void**>(&d2d_))) ||
        !d2d_) {
        return false;
    }

    const GUID dwrite_factory_iid = {
        0xb859ee5a, 0xd838, 0x4b5b,
        {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   dwrite_factory_iid,
                                   reinterpret_cast<IUnknown**>(&dwrite_))) ||
        !dwrite_) {
        return false;
    }
    if (!ui::CreateFonts(dwrite_, &fonts_)) return false;

    view_.SetPalette(palette_);
    view_.SetFonts(&fonts_);
    view_.SetDWrite(dwrite_);

    icon_small_ = static_cast<HICON>(
        LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
    icon_large_ = static_cast<HICON>(
        LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0));
    icon_edge_ = static_cast<HICON>(
        LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, 0));
    if (!icon_small_) icon_small_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    if (!icon_large_) icon_large_ = CopyIcon(icon_small_);
    if (!icon_edge_) icon_edge_ = CopyIcon(icon_large_);

    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &Application::WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = icon_large_;
    window_class.hIconSm = icon_small_;
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class)) return false;

    RebuildModel();
    panel_target_ = view_.preferred_panel_height(MaxPanelHeight());
    panel_height_ = panel_target_;
    view_.Layout(panel_height_, scale_);

    const int width_px = static_cast<int>(
        std::lround(view_.layout().window_w * scale_));
    const int height_px = static_cast<int>(
        std::lround(view_.layout().window_h * scale_));
    const POINT origin = DefaultWindowOrigin(width_px, height_px);

    DWORD ex_style = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (settings_.top_most) ex_style |= WS_EX_TOPMOST;
    hwnd_ = CreateWindowExW(ex_style, kWindowClass, kWindowTitle, WS_POPUP,
                            origin.x, origin.y, width_px, height_px, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) return false;
    view_.Attach(hwnd_);
    {
        std::lock_guard<std::mutex> lock(post_target_->mutex);
        post_target_->hwnd = hwnd_;
    }

    ChangeWindowMessageFilterEx(hwnd_, kMessageTray, MSGFLT_ALLOW, nullptr);
    if (taskbar_created_message_ != 0) {
        ChangeWindowMessageFilterEx(hwnd_, taskbar_created_message_,
                                    MSGFLT_ALLOW, nullptr);
    }
    if (!canvas_.Initialize(d2d_, width_px, height_px)) return false;

    const float window_dpi = WindowDpi(hwnd_);
    if (window_dpi != dpi_) {
        dpi_ = window_dpi;
        scale_ = dpi_ / 96.0f;
        RebuildModel();
        panel_target_ = view_.preferred_panel_height(MaxPanelHeight());
        panel_height_ = panel_target_;
        RecalculateWindow();
    }

    scan_worker_.Start();
    action_worker_.Start();
    update_worker_.Start();

    SHChangeNotifyEntry notify_entry = {};
    notify_entry.pidl = nullptr;
    notify_entry.fRecursive = TRUE;
    shell_notify_id_ = SHChangeNotifyRegister(
        hwnd_, SHCNRF_ShellLevel,
        SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED |
            SHCNE_MEDIAREMOVED | SHCNE_FREESPACE,
        kMessageShellNotify, 1, &notify_entry);

    ApplyRefreshTimer();

    if (start_hidden_) {
        in_tray_ = true;
        window_alpha_ = 1.0f;
    } else {
        window_alpha_ = 0.0f;
        PaintNow();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        ApplyTopMost();
        StartAnimation();
        ArmEmptyHideTimer();
    }

    // 在窗口显示/隐藏确定后再注册托盘图标，并延后补加一次：
    // 任务栏可能比程序启动更晚就绪，Shell_NotifyIcon 也可能在就绪前
    // 返回成功却丢弃图标。
    AddTrayIcon();
    SetTimer(hwnd_, kTimerTray, 1500, nullptr);

    RequestScan(0);
    // 启动后延迟几秒再静默检查更新，避开启动期的扫描与绘制。
    if (settings_.auto_check_updates) {
        SetTimer(hwnd_, kTimerUpdateAuto, 5000, nullptr);
    }
    return true;
}

int Application::Run() {
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

// ---------------------------------------------------------------------------
// 视图模型与窗口几何
// ---------------------------------------------------------------------------

void Application::RebuildModel() {
    ui::MainView::Model model;
    model.devices = devices_;
    model.occupants = occupants_;
    model.ejecting_disk = ejecting_disk_;
    model.busy_disk = dialog_ == ui::DialogKind::Busy ? busy_disk_ : 0;
    model.dialog = dialog_;
    model.busy_title = busy_title_;
    model.busy_scanning = busy_scanning_;
    model.busy_releasing = busy_releasing_;
    model.admin_detail = admin_detail_;
    model.confirm_dont_ask = confirm_dont_ask_;
    model.capacity_base = settings_.capacity_base;
    model.mounted_volumes.clear();
    model.mounted_volumes.reserve(mounted_volumes_.size());
    for (const auto& mounted : mounted_volumes_) {
        model.mounted_volumes.push_back(mounted.volume_path);
    }
    view_.SetModel(model);
    view_.Layout(panel_height_, scale_);
}

float Application::MaxPanelHeight() const {
    const RECT work_area = WorkAreaForWindow(hwnd_);
    const float screen =
        (work_area.bottom - work_area.top) / (scale_ > 0 ? scale_ : 1.0f);
    if (screen <= 0.0f) return 600.0f;
    const float percent = static_cast<float>(settings_.main_height_percent) / 100.0f;
    const float limit = screen * percent - view_.kShadowPad * 2.0f;
    const float available = screen - view_.kShadowPad * 2.0f - 40.0f;
    return std::max(120.0f, std::min(available, limit));
}

void Application::UpdatePanelTarget() {
    RebuildModel();
    const float desired = view_.preferred_panel_height(MaxPanelHeight());
    if (std::fabs(desired - panel_target_) > 0.4f) {
        panel_target_ = desired;
        StartAnimation();
    }
}

void Application::RecalculateWindow() {
    if (!hwnd_) return;
    view_.Layout(panel_height_, scale_);
    const int width_px = static_cast<int>(
        std::lround(view_.layout().window_w * scale_));
    const int height_px = static_cast<int>(
        std::lround(view_.layout().window_h * scale_));
    if (width_px <= 0 || height_px <= 0) return;

    RECT current = {};
    GetWindowRect(hwnd_, &current);
    const RECT work_area = WorkAreaForWindow(hwnd_);
    int left = current.left;
    int top = current.top;
    // 左下 / 右下定位时窗口应贴住底边：高度变化以底边为锚点，否则内容变矮时
    // 空出的高度会留在窗口下方，表现为整窗向上跑。
    if (settings_.position_mode == 0 &&
        (settings_.corner == 1 || settings_.corner == 3)) {
        top = current.bottom - height_px;
    }
    if (left + width_px > work_area.right - 8) {
        left = work_area.right - 8 - width_px;
    }
    if (left < work_area.left + 8) left = work_area.left + 8;
    if (top + height_px > work_area.bottom - 8) {
        top = work_area.bottom - 8 - height_px;
    }
    if (top < work_area.top + 8) top = work_area.top + 8;

    const int current_width = current.right - current.left;
    const int current_height = current.bottom - current.top;
    if (left != current.left || top != current.top ||
        width_px != current_width || height_px != current_height) {
        SetWindowPos(hwnd_, nullptr, left, top, width_px, height_px,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    canvas_.Resize(width_px, height_px);
}

void Application::PaintNow() {
    if (!hwnd_ || !canvas_.valid()) return;
    view_.Layout(panel_height_, scale_);
    canvas_.Begin(dpi_);
    view_.Paint(canvas_, dpi_, window_alpha_);
    canvas_.End();
    const float alpha = std::min(std::max(window_alpha_, 0.0f), 1.0f);
    canvas_.Present(hwnd_,
                    static_cast<unsigned char>(std::lround(alpha * 255.0f)));
}

void Application::StartAnimation() {
    if (!hwnd_) return;
    if (animating_) return;
    animating_ = true;
    // 16ms 会被系统时钟粒度向上取整到两个 tick（约 31ms，只有 32fps），
    // 按 15ms 申请才能落在单个时钟 tick（15.6ms）上。
    SetTimer(hwnd_, kTimerAnim, 15, nullptr);
}

void Application::TickAnimation() {
    if (!hwnd_) return;
    const bool view_more = view_.Tick();
    bool more = view_more;
    const bool panel_more = Ease(panel_height_, panel_target_, 0.20f, 0.35f);
    const bool alpha_more = Ease(window_alpha_, 1.0f, 0.11f, 0.01f);
    more |= panel_more;
    more |= alpha_more;

    if (std::fabs(slide_x_) > 0.5f || std::fabs(slide_y_) > 0.5f) {
        more |= Ease(slide_x_, 0.0f, 0.28f, 0.5f);
        more |= Ease(slide_y_, 0.0f, 0.28f, 0.5f);
        SetWindowPos(hwnd_, nullptr, restore_x_ + static_cast<int>(slide_x_),
                     restore_y_ + static_cast<int>(slide_y_), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        // 窗口尺寸只跟面板高度有关；高度没变时不必每帧重新 SetWindowPos，
        // 否则会把用户正在拖动的窗口按工作区边界拉回去，产生回弹抖动。
        if (panel_more) RecalculateWindow();
    }

    if (!more) {
        KillTimer(hwnd_, kTimerAnim);
        animating_ = false;
        PaintNow();
        return;
    }
    PaintNow();
}

void Application::UpdateTheme() {
    const bool dark = ui::IsSystemDarkMode();
    if (dark == palette_.dark) return;
    palette_ = ui::MakePalette(dark);
    view_.SetPalette(palette_);
    settings_window_.SetPalette(palette_);
    about_window_.SetPalette(palette_);
    edge_bar_.SetTheme(dark);
    PaintNow();
}

void Application::ApplyTopMost() {
    if (!hwnd_) return;
    // 主窗口的置顶只由「始终置顶」决定，不再因为设置窗口打开而让出。
    // 设置窗口与主窗口一起置顶，并重新压到主窗口之上，避免被主窗口盖住。
    SetWindowPos(hwnd_, settings_.top_most ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0,
                 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    settings_window_.SetTopMost(settings_.top_most);
    about_window_.SetTopMost(settings_.top_most);
}

void Application::ApplyRefreshTimer() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimerPoll);
    if (settings_.refresh_seconds > 0) {
        SetTimer(hwnd_, kTimerPoll,
                 static_cast<UINT>(settings_.refresh_seconds) * 1000, nullptr);
    }
}

void Application::ApplySettings(const Settings& settings) {
    const bool filter_changed =
        settings_.show_hidden_partitions != settings.show_hidden_partitions ||
        settings_.show_esp_partitions != settings.show_esp_partitions ||
        settings_.show_msr_partitions != settings.show_msr_partitions ||
        settings_.show_other_hidden_partitions !=
            settings.show_other_hidden_partitions;
    const bool height_changed =
        settings_.main_height_percent != settings.main_height_percent;
    const bool position_changed =
        settings_.position_mode != settings.position_mode ||
        settings_.corner != settings.corner;

    settings_ = settings;
    settings_.Normalize();
    USB_LOG_INFO(
        L"app",
        L"设置变更：filter=%d height=%d position=%d top_most=%d "
        L"pos_mode=%d corner=%d close_action=%d refresh=%d edge=%d",
        filter_changed ? 1 : 0, height_changed ? 1 : 0,
        position_changed ? 1 : 0, settings_.top_most ? 1 : 0,
        settings_.position_mode, settings_.corner, settings_.close_action,
        settings_.refresh_seconds, settings_.edge_collapse ? 1 : 0);

    if (filter_changed) {
        // 分区被隐藏后不再保留本进程分配的盘符，避免留下说不清的挂载。
        if (!settings_.show_hidden_partitions) {
            UnmountManagedVolumes(PartitionKind::Normal, true, true);
        } else {
            if (!settings_.show_esp_partitions) {
                UnmountManagedVolumes(PartitionKind::Esp, false, true);
            }
            if (!settings_.show_msr_partitions) {
                UnmountManagedVolumes(PartitionKind::Msr, false, true);
            }
            if (!settings_.show_other_hidden_partitions) {
                UnmountManagedVolumes(PartitionKind::Normal, false, true);
            }
        }
    }

    ApplyTopMost();
    ApplyRefreshTimer();
    if (!settings_.auto_check_updates) KillTimer(hwnd_, kTimerUpdateAuto);

    if (position_changed) ApplyWindowPosition();
    store_.Save(settings_);

    if (!settings_.edge_collapse && edge_collapsed_) {
        RestoreFromEdgeBar();
    }

    if (filter_changed) RequestScan(0);
    if (height_changed) UpdatePanelTarget();

    RebuildModel();
    ArmEmptyHideTimer();
    StartAnimation();
    PaintNow();
}

void Application::ShowSettings() {
    settings_window_.Open(
        instance_, hwnd_, d2d_, dwrite_, &fonts_, palette_, settings_,
        [this](const Settings& settings) { ApplySettings(settings); });
    ApplyTopMost();
}

void Application::ShowAbout() {
    // 重新打开时让任何在途结果失效，避免旧结果落到新窗口上。
    ++update_generation_;
    update_busy_ = false;
    about_window_.Open(
        instance_, hwnd_, d2d_, dwrite_, &fonts_, palette_, kAboutVersion,
        [this]() { StartUpdateCheck(false); },
        [this]() {
            ++update_generation_;
            update_busy_ = false;
        });
    ApplyTopMost();
}

void Application::StartUpdateCheck(bool silent) {
    if (!hwnd_) return;
    if (update_busy_) return;
    update_busy_ = true;
    update_silent_ = silent;
    const std::uint64_t generation = ++update_generation_;
    const std::shared_ptr<PostTarget> target = post_target_;
    USB_LOG_INFO(L"update",
                 L"开始检查更新：current=%s silent=%d dev=%d",
                 kAppVersion, silent ? 1 : 0, kDevBuild ? 1 : 0);
    update_worker_.Post([target, generation]() {
        auto payload = std::make_unique<UpdatePayload>();
        payload->generation = generation;
        payload->result = CheckLatestRelease(kAppVersion);
        PostPayload(target, kMessageUpdateDone, std::move(payload));
    });
}

void Application::ShowUpdateBalloon(const std::wstring& latest_version) {
    if (!hwnd_ || !tray_registered_) return;
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    const std::wstring title = L"发现新版本 v" + latest_version;
    const std::wstring text =
        L"打开托盘菜单中的「关于」即可查看并前往下载。";
    std::copy_n(title.begin(),
                std::min(title.size(), std::size(data.szInfoTitle) - 1),
                data.szInfoTitle);
    std::copy_n(text.begin(),
                std::min(text.size(), std::size(data.szInfo) - 1),
                data.szInfo);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// ---------------------------------------------------------------------------
// 设备扫描
// ---------------------------------------------------------------------------

DeviceFilter Application::CurrentFilter() const {
    DeviceFilter filter;
    filter.include_hidden = settings_.show_hidden_partitions;
    filter.include_esp = settings_.show_esp_partitions;
    filter.include_msr = settings_.show_msr_partitions;
    filter.include_other_hidden = settings_.show_other_hidden_partitions;
    return filter;
}

void Application::RequestScan(int delay_ms) {
    if (!hwnd_) return;
    if (delay_ms > 0) {
        SetTimer(hwnd_, kTimerScan, static_cast<UINT>(delay_ms), nullptr);
        return;
    }
    StartScan();
}

void Application::StartScan() {
    if (scan_in_flight_) {
        scan_pending_ = true;
        return;
    }
    scan_in_flight_ = true;
    const std::shared_ptr<PostTarget> target = post_target_;
    const DeviceFilter filter = CurrentFilter();
    scan_worker_.Post([target, filter]() {
        auto devices = std::make_shared<const std::vector<Device>>(
            EnumerateDevices(filter));
        auto payload = std::make_unique<ScanPayload>();
        payload->devices = std::move(devices);
        PostPayload(target, kMessageScanDone, std::move(payload));
    });
}

void Application::ApplyDevices(
    std::shared_ptr<const std::vector<Device>> devices) {
    const bool was_empty = !devices_ || devices_->empty();
    devices_ = devices ? std::move(devices)
                       : std::make_shared<const std::vector<Device>>();
    PruneMountedVolumes();

    RebuildModel();
    UpdatePanelTarget();

    if (devices_->empty()) {
        ArmEmptyHideTimer();
    } else {
        if (hwnd_) KillTimer(hwnd_, kTimerEmptyHide);
        empty_hide_armed_ = false;
        if (was_empty && in_tray_) ShowFromTray();
    }

    StartAnimation();
    PaintNow();
}

// ---------------------------------------------------------------------------
// 用户动作
// ---------------------------------------------------------------------------

void Application::OnAction(const ui::ViewAction& action) {
    switch (action.kind) {
        case ui::ActionKind::Close:
            RequestClose();
            break;
        case ui::ActionKind::ConfirmExit:
            ConfirmExit();
            break;
        case ui::ActionKind::CancelDialog:
            CloseDialog();
            break;
        case ui::ActionKind::ToggleDontAsk:
            confirm_dont_ask_ = !confirm_dont_ask_;
            RebuildModel();
            PaintNow();
            break;
        case ui::ActionKind::Eject:
            StartEject(action.device);
            break;
        case ui::ActionKind::Open:
            OpenPartition(action.device, action.partition);
            break;
        case ui::ActionKind::Mount:
            MountPartition(action.device, action.partition);
            break;
        case ui::ActionKind::Unmount:
            UnmountPartition(action.device, action.partition);
            break;
        case ui::ActionKind::ReleaseOccupants:
            ReleaseOccupantsAndRetry();
            break;
        case ui::ActionKind::Elevate:
            RunElevated();
            break;
        case ui::ActionKind::DragEnded:
            RecordWindowPosition();
            CheckDropPosition();
            break;
        case ui::ActionKind::None:
            break;
    }
}

void Application::OpenPartition(std::size_t device_index,
                                std::size_t partition_index) {
    if (!devices_ || device_index >= devices_->size()) return;
    const Device& device = (*devices_)[device_index];
    if (partition_index >= device.partitions.size()) return;
    const Partition& part = device.partitions[partition_index];

    std::wstring root = part.root;
    if (root.empty() && part.letter != 0) {
        root = std::wstring(1, part.letter) + L":\\";
    }
    if (root.empty()) return;

    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowToast(L"无法打开 " + root + L"，设备可能已被拔出。", false);
        RequestScan(300);
        return;
    }

    // ESP 等受保护卷只允许管理员浏览。「列出目录」探测等价于资源管理器
    // 浏览所需权限；主程序已是管理员时探测会通过，但 Explorer 通常以中等
    // 完整性运行，仍会被卷的安全控制拒绝，因此 ESP 一律走提权窗口。
    const bool elevated = IsProcessElevated();
    const bool needs_elevated_open =
        elevated ? part.kind == PartitionKind::Esp
                 : storage::ProbeDirectoryAccess(root) == ERROR_ACCESS_DENIED;
    if (needs_elevated_open) {
        if (elevated) {
            if (LaunchExplorerSeparate(root)) {
                ShowToast(L"已用管理员权限打开 " + root +
                              L"（资源管理器为管理员窗口）。",
                          true);
            } else {
                ShowToast(L"打开 " + root + L" 失败，无法启动资源管理器。",
                          false);
            }
            return;
        }
        ShowElevationPrompt(
            AdminAction::Open,
            L"将用管理员权限打开「" + PartitionMountLabel(part) +
                L"」（管理员窗口）。",
            0, L"", {}, L"", 0, root);
        return;
    }

    const HINSTANCE result =
        ShellExecuteW(hwnd_, L"open", root.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) return;

    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(root.c_str());
    if (pidl) {
        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return;
    }
    ShowToast(L"打开 " + root + L" 失败，请手动在资源管理器中打开。", false);
}

void Application::MountPartition(std::size_t device_index,
                                 std::size_t partition_index) {
    if (action_busy_) return;
    if (!devices_ || device_index >= devices_->size()) return;
    const Device& device = (*devices_)[device_index];
    if (partition_index >= device.partitions.size()) return;
    const Partition part = device.partitions[partition_index];

    if (part.letter != 0) return;
    if (part.kind == PartitionKind::Msr || part.volume_path.empty()) {
        ShowToast(L"该分区不支持分配盘符。", false);
        return;
    }

    wchar_t letter = storage::FindVolumeDriveLetter(part.volume_path);
    if (letter == 0) {
        const storage::MountResult result =
            storage::AssignDriveLetter(part.volume_path);
        if (!result.ok()) {
            if (result.status == storage::MountStatus::AccessDenied &&
                !IsProcessElevated()) {
                ShowElevationPrompt(AdminAction::Mount,
                                    L"将把「" + PartitionMountLabel(part) +
                                        L"」挂载为可用盘符。",
                                    0, L"", {}, part.volume_path, 0);
            } else {
                const std::wstring feedback =
                    result.status == storage::MountStatus::AccessDenied
                        ? L"即使以管理员身份运行，仍无法挂载该分区，请检查"
                          L"系统策略后重试。"
                        : (result.message.empty() ? L"挂载失败。"
                                                  : result.message);
                ShowToast(feedback, false);
            }
            return;
        }
        letter = result.letter;
    }

    mounted_volumes_.erase(
        std::remove_if(mounted_volumes_.begin(), mounted_volumes_.end(),
                       [&part](const MountedVolume& mounted) {
                           return mounted.volume_path == part.volume_path;
                       }),
        mounted_volumes_.end());
    mounted_volumes_.push_back(
        MountedVolume{part.volume_path, letter, part.kind});

    auto copy = std::make_shared<std::vector<Device>>(*devices_);
    for (auto& candidate : *copy) {
        for (auto& item : candidate.partitions) {
            if (item.volume_path != part.volume_path) continue;
            item.letter = letter;
            item.root = std::wstring(1, letter) + L":\\";
        }
    }
    devices_ = std::move(copy);

    ShowToast(L"已将 " + PartitionMountLabel(part) + L" 挂载为 " +
                  std::wstring(1, letter) +
                  L":（等同于磁盘管理中分配盘符）",
              true);
    UpdatePanelTarget();
    StartAnimation();
    PaintNow();
    RequestScan(500);
}

void Application::UnmountPartition(std::size_t device_index,
                                   std::size_t partition_index) {
    if (action_busy_) return;
    if (!devices_ || device_index >= devices_->size()) return;
    const Device& device = (*devices_)[device_index];
    if (partition_index >= device.partitions.size()) return;
    const Partition part = device.partitions[partition_index];
    if (part.letter == 0) return;

    const storage::MountResult result =
        storage::RemoveDriveLetter(part.letter);
    if (!result.ok()) {
        if (result.status == storage::MountStatus::AccessDenied &&
            !IsProcessElevated()) {
            ShowElevationPrompt(
                AdminAction::Unmount,
                L"将取消挂载 " + std::wstring(1, part.letter) + L":。", 0, L"",
                {}, part.volume_path, part.letter);
        } else {
            const std::wstring feedback =
                result.status == storage::MountStatus::AccessDenied
                    ? L"即使以管理员身份运行，仍无法取消挂载该分区，"
                      L"请稍后重试。"
                    : (result.message.empty() ? L"取消挂载失败。"
                                              : result.message);
            ShowToast(feedback, false);
        }
        return;
    }

    mounted_volumes_.erase(
        std::remove_if(mounted_volumes_.begin(), mounted_volumes_.end(),
                       [&part](const MountedVolume& mounted) {
                           return mounted.volume_path == part.volume_path;
                       }),
        mounted_volumes_.end());

    auto copy = std::make_shared<std::vector<Device>>(*devices_);
    for (auto& candidate : *copy) {
        for (auto& item : candidate.partitions) {
            if (item.volume_path != part.volume_path) continue;
            item.letter = 0;
            item.root.clear();
        }
    }
    devices_ = std::move(copy);

    ShowToast(L"已取消挂载 " + std::wstring(1, result.letter) + L":", true);
    UpdatePanelTarget();
    StartAnimation();
    PaintNow();
    RequestScan(400);
}

void Application::UnmountManagedVolumes(PartitionKind kind, bool all,
                                        bool notify) {
    bool failed = false;
    for (auto it = mounted_volumes_.begin(); it != mounted_volumes_.end();) {
        if (!all && it->kind != kind) {
            ++it;
            continue;
        }
        const storage::MountResult result =
            storage::RemoveDriveLetter(it->letter);
        if (!result.ok()) failed = true;
        it = mounted_volumes_.erase(it);
    }
    if (failed && notify) {
        ShowToast(L"有分区取消挂载失败，可能仍在资源管理器中可见。", false);
    }
}

void Application::PruneMountedVolumes() {
    if (mounted_volumes_.empty()) return;
    for (auto it = mounted_volumes_.begin(); it != mounted_volumes_.end();) {
        // 只根据卷是否还存在来清理。盘符可能暂时读不到（扫描快照较早或
        // 被外部工具取消），但记录保留下来才能在退出时继续兜底清理。
        bool present = false;
        if (devices_) {
            for (const auto& device : *devices_) {
                for (const auto& part : device.partitions) {
                    if (part.volume_path == it->volume_path) {
                        present = true;
                        break;
                    }
                }
                if (present) break;
            }
        }
        if (present) {
            ++it;
        } else {
            it = mounted_volumes_.erase(it);
        }
    }
}

void Application::RequestClose() {
    // 弹层打开时，关闭请求只由弹层自己的按钮处理，避免用退出确认
    // 覆盖正在进行的占用/权限流程。
    if (dialog_ != ui::DialogKind::None) return;

    if (settings_.close_action == 1) {
        HideToTray();
        return;
    }
    if (settings_.skip_close_confirm) {
        DestroyWindow(hwnd_);
        return;
    }
    dialog_ = ui::DialogKind::ConfirmClose;
    confirm_dont_ask_ = false;
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    RebuildModel();
    StartAnimation();
    PaintNow();
}

void Application::ConfirmExit() {
    if (confirm_dont_ask_) {
        settings_.skip_close_confirm = true;
        store_.Save(settings_);
    }
    DestroyWindow(hwnd_);
}

void Application::CloseDialog() {
    if (dialog_ == ui::DialogKind::None) return;

    // 取消占用扫描时，让在途结果立即失效，并解锁后续操作；
    // 正在「解除占用 + 弹出」时不允许关闭（按钮被禁用），避免出现
    // “界面已取消、后台仍把设备弹出”的歧义。
    if (action_busy_ && busy_scanning_ && !busy_releasing_) {
        ++action_generation_;
        action_busy_ = false;
        if (hwnd_) KillTimer(hwnd_, kTimerAction);
    }

    dialog_ = ui::DialogKind::None;
    if (hwnd_) KillTimer(hwnd_, kTimerOccupantsRefresh);
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    busy_title_.clear();
    busy_targets_.clear();
    busy_disk_ = 0;
    busy_scanning_ = false;
    busy_releasing_ = false;
    action_busy_ = false;
    admin_action_ = AdminAction::None;
    admin_detail_.clear();
    admin_title_.clear();
    admin_letters_.clear();
    admin_disk_ = 0;
    admin_volume_path_.clear();
    admin_letter_ = 0;
    admin_root_.clear();
    confirm_dont_ask_ = false;
    RebuildModel();
    StartAnimation();
    PaintNow();
}

// ---------------------------------------------------------------------------
// 弹出 / 占用 / 提权
// ---------------------------------------------------------------------------

void Application::StartEject(std::size_t device_index) {
    if (!devices_ || device_index >= devices_->size()) return;
    if (action_busy_) return;

    const Device& device = (*devices_)[device_index];
    if (device.disk_number == ejecting_disk_) return;

    std::vector<EjectTarget> targets;
    for (const auto& part : device.partitions) {
        if (!part.volume_path.empty()) {
            targets.push_back(EjectTarget{part.volume_path, part.letter});
        }
    }
    if (targets.empty()) {
        ShowToast(L"该设备没有可弹出的分区，可能已被拔出。", false);
        RequestScan(300);
        return;
    }

    const std::uint32_t disk_number = device.disk_number;
    const std::wstring title = device.title;
    const std::uint64_t generation = ++action_generation_;

    dialog_ = ui::DialogKind::None;
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    ejecting_disk_ = disk_number;
    action_busy_ = true;
    if (hwnd_) SetTimer(hwnd_, kTimerAction, kActionTimeoutEjectMs, nullptr);

    RebuildModel();
    StartAnimation();
    PaintNow();

    const std::shared_ptr<PostTarget> target = post_target_;
    action_worker_.Post([target, generation, disk_number, title, targets]() {
        auto payload = std::make_unique<EjectPayload>();
        payload->generation = generation;
        payload->disk_number = disk_number;
        payload->title = title;
        payload->targets = targets;
        payload->result = EjectDevice(disk_number, title, targets);
        PostPayload(target, kMessageEjectDone, std::move(payload));
    });
}

void Application::StartEjectByDisk(std::uint32_t disk_number) {
    if (!devices_) return;
    for (std::size_t i = 0; i < devices_->size(); ++i) {
        if ((*devices_)[i].disk_number == disk_number) {
            StartEject(i);
            return;
        }
    }
    ShowToast(L"设备已不在列表中，可能已经弹出。", true);
    RequestScan(300);
}

void Application::ShowBusyDialog(std::uint32_t disk_number,
                                 const std::wstring& title,
                                 const std::vector<EjectTarget>& targets) {
    dialog_ = ui::DialogKind::Busy;
    if (hwnd_) KillTimer(hwnd_, kTimerOccupantsRefresh);
    occupants_refresh_ticks_ = 0;
    busy_disk_ = disk_number;
    busy_title_ = title;
    busy_targets_ = targets;
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    busy_scanning_ = true;
    busy_releasing_ = false;
    RebuildModel();
    StartAnimation();
    PaintNow();
    StartOccupantScan();
}

void Application::StartOccupantScan() {
    if (!hwnd_ || action_busy_ || dialog_ != ui::DialogKind::Busy) return;
    const std::vector<wchar_t> letters = LettersFromTargets(busy_targets_);
    const std::uint64_t generation = ++action_generation_;

    busy_scanning_ = true;
    busy_releasing_ = false;
    action_busy_ = true;
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    SetTimer(hwnd_, kTimerAction, kActionTimeoutScanMs, nullptr);

    RebuildModel();
    StartAnimation();
    PaintNow();

    const std::shared_ptr<PostTarget> target = post_target_;
    action_worker_.Post([target, generation, letters]() {
        auto payload = std::make_unique<OccupantsPayload>();
        payload->generation = generation;
        payload->occupants = std::make_shared<const std::vector<Occupant>>(
            FindOccupants(letters, &payload->complete));
        PostPayload(target, kMessageOccupantsDone, std::move(payload));
    });
}

void Application::CheckOccupantsRefresh() {
    if (!hwnd_ || dialog_ != ui::DialogKind::Busy || action_busy_ ||
        busy_scanning_ || busy_releasing_ || !occupants_ ||
        occupants_->empty()) {
        return;
    }
    ++occupants_refresh_ticks_;
    // 每秒只做便宜的存活检查；有进程退出（或每 5 拍）才做一次完整句柄扫描。
    // 整扫实测约 120ms，不能按 1 秒直接整扫。
    const bool periodic = (occupants_refresh_ticks_ % 5) == 0;
    if (!periodic && OccupantsStillAlive(*occupants_)) return;
    StartOccupantRefresh();
}

void Application::StartOccupantRefresh() {
    if (!hwnd_ || dialog_ != ui::DialogKind::Busy || action_busy_ ||
        busy_scanning_ || busy_releasing_) {
        return;
    }
    const std::vector<wchar_t> letters = LettersFromTargets(busy_targets_);
    if (letters.empty()) return;

    const std::uint64_t generation = ++action_generation_;
    const std::shared_ptr<PostTarget> target = post_target_;
    action_worker_.Post([target, generation, letters]() {
        auto payload = std::make_unique<OccupantsPayload>();
        payload->generation = generation;
        payload->refresh = true;
        payload->occupants = std::make_shared<const std::vector<Occupant>>(
            FindOccupants(letters, &payload->complete));
        PostPayload(target, kMessageOccupantsDone, std::move(payload));
    });
}

// 只有确实有占用条目展示时才做后台复检，避免无意义的后台扫描。
void Application::SyncOccupantsRefreshTimer() {
    if (!hwnd_) return;
    const bool active = dialog_ == ui::DialogKind::Busy && occupants_ &&
                        !occupants_->empty() && !busy_releasing_;
    if (!active) {
        KillTimer(hwnd_, kTimerOccupantsRefresh);
        return;
    }
    const int seconds = std::max(1, settings_.occupants_refresh_seconds);
    SetTimer(hwnd_, kTimerOccupantsRefresh,
             static_cast<UINT>(seconds) * 1000, nullptr);
}

void Application::ReleaseOccupantsAndRetry() {
    if (dialog_ != ui::DialogKind::Busy || busy_scanning_ || busy_releasing_ ||
        action_busy_) {
        return;
    }

    if (!occupants_ || occupants_->empty()) {
        const std::uint32_t disk_number = busy_disk_;
        CloseDialog();
        StartEjectByDisk(disk_number);
        return;
    }

    std::vector<DWORD> pids;
    pids.reserve(occupants_->size());
    for (const auto& occupant : *occupants_) pids.push_back(occupant.pid);
    const std::vector<wchar_t> letters = LettersFromTargets(busy_targets_);
    const std::uint32_t disk_number = busy_disk_;
    const std::wstring title = busy_title_;
    const std::vector<EjectTarget> targets = busy_targets_;
    const std::uint64_t generation = ++action_generation_;

    busy_releasing_ = true;
    action_busy_ = true;
    KillTimer(hwnd_, kTimerOccupantsRefresh);
    SetTimer(hwnd_, kTimerAction, kActionTimeoutReleaseMs, nullptr);

    RebuildModel();
    StartAnimation();
    PaintNow();

    const std::shared_ptr<PostTarget> target = post_target_;
    action_worker_.Post([target, generation, disk_number, title, targets, pids,
                         letters]() {
        auto payload = std::make_unique<EjectPayload>();
        payload->generation = generation;
        payload->disk_number = disk_number;
        payload->title = title;
        payload->targets = targets;
        payload->from_release = true;

        const ReleaseReport report = ReleaseOccupants(pids, letters);
        payload->released = report.released();
        payload->denied = report.denied;
        payload->explorer = report.explorer;
        payload->skipped = report.skipped;

        if (report.released() == 0) {
            if (report.denied > 0) {
                payload->result.status = EjectStatus::AccessDenied;
                payload->result.message =
                    L"解除占用需要管理员权限。点击「确认」后会在管理员模式下"
                    L"继续解除占用并弹出。";
            } else if (report.explorer > 0) {
                payload->result.status = EjectStatus::Busy;
                payload->result.message =
                    L"请先关闭打开该设备的资源管理器窗口后重试。";
            } else if (report.skipped > 0) {
                payload->result.status = EjectStatus::Busy;
                payload->result.message =
                    L"占用来自系统关键进程，无法自动结束，请手动关闭后重试。";
            } else {
                payload->result.status = EjectStatus::Busy;
                payload->result.message = L"没有找到可解除的占用进程。";
            }
        } else {
            Sleep(300);  // 等系统释放被结束进程的句柄
            payload->result = EjectDevice(disk_number, title, targets);
        }
        PostPayload(target, kMessageEjectDone, std::move(payload));
    });
}

void Application::ShowElevationPrompt(AdminAction action,
                                      const std::wstring& detail,
                                      std::uint32_t disk_number,
                                      const std::wstring& title,
                                      const std::vector<wchar_t>& letters,
                                      const std::wstring& volume_path,
                                      wchar_t letter,
                                      const std::wstring& root) {
    dialog_ = ui::DialogKind::Admin;
    USB_LOG_INFO(L"elevation",
                 L"弹出管理员确认：action=%d disk=%u letter=%c detail=%s",
                 static_cast<int>(action), disk_number,
                 letter ? letter : L'-', detail.c_str());
    admin_action_ = action;
    admin_detail_ = detail;
    admin_disk_ = disk_number;
    admin_title_ = title;
    admin_letters_ = letters;
    admin_volume_path_ = volume_path;
    admin_letter_ = letter;
    admin_root_ = root;
    busy_scanning_ = false;
    busy_releasing_ = false;
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    RebuildModel();
    StartAnimation();
    PaintNow();
}

void Application::RunElevated() {
    if (action_busy_) return;
    const AdminAction action = admin_action_;
    if (action == AdminAction::None) return;

    const std::uint32_t disk_number = admin_disk_;
    const std::wstring title = admin_title_;
    const std::vector<wchar_t> letters = admin_letters_;
    const std::wstring volume_path = admin_volume_path_;
    const wchar_t letter = admin_letter_;
    const std::wstring root = admin_root_;

    if ((action == AdminAction::Eject && letters.empty()) ||
        (action == AdminAction::Mount && volume_path.empty()) ||
        (action == AdminAction::Unmount && letter == 0) ||
        (action == AdminAction::Open && root.empty())) {
        CloseDialog();
        ShowToast(L"目标设备或分区已不在，操作未执行。", false);
        return;
    }

    const std::wstring executable = ModulePath();
    if (executable.empty()) {
        ShowToast(L"无法获取程序路径，管理员模式启动失败。", false);
        return;
    }

    HelperRequest request;
    if (action == AdminAction::Mount) {
        request.operation = HelperOperation::Mount;
        request.volume_path = volume_path;
    } else if (action == AdminAction::Unmount) {
        request.operation = HelperOperation::Unmount;
        request.letter = letter;
    } else if (action == AdminAction::Open) {
        request.operation = HelperOperation::Open;
        request.root = root;
    } else {
        request.operation = HelperOperation::Eject;
        request.disk_number = disk_number;
        request.letters = letters;
    }
    request.result_path = MakeHelperResultPath();
    if (request.result_path.empty()) {
        ShowToast(L"无法创建临时文件，管理员模式启动失败。", false);
        return;
    }

    const std::uint64_t generation = ++action_generation_;
    dialog_ = ui::DialogKind::None;
    admin_action_ = AdminAction::None;
    admin_detail_.clear();
    admin_disk_ = 0;
    admin_title_.clear();
    admin_letters_.clear();
    admin_volume_path_.clear();
    admin_letter_ = 0;
    admin_root_.clear();
    occupants_ = std::make_shared<const std::vector<Occupant>>();
    action_busy_ = true;
    // 提权链路由 LaunchHelperAndWait 自己限时；UAC 等待期间不在这里
    // 中断，避免 UAC 稍后确认后操作已完成、主程序却丢掉结果。

    if (action == AdminAction::Mount) {
        ShowToast(L"已请求管理员权限，正在挂载分区…", true);
    } else if (action == AdminAction::Unmount) {
        ShowToast(L"已请求管理员权限，正在取消挂载…", true);
    } else if (action == AdminAction::Open) {
        ShowToast(L"已请求管理员权限，正在打开 " + root + L"…", true);
    } else {
        ShowToast(L"已请求管理员权限，正在解除占用并弹出…", true);
    }
    RebuildModel();
    StartAnimation();
    PaintNow();

    const std::shared_ptr<PostTarget> target = post_target_;
    action_worker_.Post([target, generation, disk_number, title, letters,
                         volume_path, letter, root, executable, request]() {
        auto payload = std::make_unique<HelperPayload>();
        payload->generation = generation;
        payload->operation = request.operation;
        payload->disk_number = disk_number;
        payload->title = title;
        payload->letters = letters;
        payload->volume_path = volume_path;
        payload->letter = letter;
        payload->root = root;
        payload->outcome = LaunchHelperAndWait(executable, request,
                                               kHelperProcessTimeoutMs);
        PostPayload(target, kMessageHelperDone, std::move(payload));
    });
}

void Application::ApplyElevatedMount(const std::wstring& volume_path,
                                     wchar_t letter) {
    if (volume_path.empty() || letter == 0) return;

    PartitionKind kind = PartitionKind::Normal;
    if (devices_) {
        for (const auto& device : *devices_) {
            for (const auto& part : device.partitions) {
                if (part.volume_path == volume_path) {
                    kind = part.kind;
                    break;
                }
            }
        }
    }

    mounted_volumes_.erase(
        std::remove_if(mounted_volumes_.begin(), mounted_volumes_.end(),
                       [&volume_path](const MountedVolume& mounted) {
                           return mounted.volume_path == volume_path;
                       }),
        mounted_volumes_.end());
    mounted_volumes_.push_back(MountedVolume{volume_path, letter, kind});

    if (devices_) {
        auto copy = std::make_shared<std::vector<Device>>(*devices_);
        for (auto& device : *copy) {
            for (auto& part : device.partitions) {
                if (part.volume_path != volume_path) continue;
                part.letter = letter;
                part.root = std::wstring(1, letter) + L":\\";
            }
        }
        devices_ = std::move(copy);
    }

    UpdatePanelTarget();
    StartAnimation();
    PaintNow();
}

void Application::ApplyElevatedUnmount(wchar_t letter) {
    if (letter == 0) return;

    mounted_volumes_.erase(
        std::remove_if(mounted_volumes_.begin(), mounted_volumes_.end(),
                       [letter](const MountedVolume& mounted) {
                           return mounted.letter == letter;
                       }),
        mounted_volumes_.end());

    if (devices_) {
        auto copy = std::make_shared<std::vector<Device>>(*devices_);
        for (auto& device : *copy) {
            for (auto& part : device.partitions) {
                if (part.letter != letter) continue;
                part.letter = 0;
                part.root.clear();
            }
        }
        devices_ = std::move(copy);
    }

    UpdatePanelTarget();
    StartAnimation();
    PaintNow();
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------

void Application::AddTrayIcon() {
    if (!hwnd_) return;
    if (!icon_small_) {
        icon_small_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    }
    tray_data_ = {};
    tray_data_.cbSize = sizeof(tray_data_);
    tray_data_.hWnd = hwnd_;
    tray_data_.uID = 1;
    tray_data_.uFlags =
        NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_STATE;
    tray_data_.uCallbackMessage = kMessageTray;
    tray_data_.hIcon = icon_small_;
    // 明确要求“非隐藏”状态。Windows 11 仍可能把新图标放进溢出区，
    // 但至少不会因为遗留的 NIS_HIDDEN 状态直接消失。
    tray_data_.dwState = 0;
    tray_data_.dwStateMask = NIS_HIDDEN;
    const std::wstring tip = kWindowTitle;
    const std::size_t tip_capacity = std::size(tray_data_.szTip);
    const std::size_t tip_length =
        std::min(tip.size(), tip_capacity - 1);
    std::copy_n(tip.begin(), tip_length, tray_data_.szTip);
    tray_data_.szTip[tip_length] = L'\0';

    Shell_NotifyIconW(NIM_DELETE, &tray_data_);
    if (!Shell_NotifyIconW(NIM_ADD, &tray_data_)) {
        tray_registered_ = false;
        KillTimer(hwnd_, kTimerTrayHealth);
        SetTimer(hwnd_, kTimerTray, 2000, nullptr);
        return;
    }
    tray_registered_ = true;
    KillTimer(hwnd_, kTimerTray);
    SetTimer(hwnd_, kTimerTrayHealth, 15000, nullptr);
    tray_data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray_data_);

    // 再显式清一次 NIS_HIDDEN：部分系统会把“曾经隐藏”的状态记住，
    // 仅靠 NIM_ADD 时的 dwState 不一定能恢复可见。
    tray_data_.uFlags = NIF_STATE;
    tray_data_.dwState = 0;
    tray_data_.dwStateMask = NIS_HIDDEN;
    Shell_NotifyIconW(NIM_MODIFY, &tray_data_);
}

void Application::RemoveTrayIcon() {
    if (!hwnd_) return;
    tray_registered_ = false;
    KillTimer(hwnd_, kTimerTrayHealth);
    KillTimer(hwnd_, kTimerTray);
    tray_data_.cbSize = sizeof(tray_data_);
    tray_data_.hWnd = hwnd_;
    tray_data_.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &tray_data_);
}

void Application::ShowTrayMenu() {
    if (!hwnd_ || tray_menu_open_) return;
    POINT cursor = {};
    GetCursorPos(&cursor);
    ui::TrayMenu menu;
    tray_menu_open_ = true;
    const int choice =
        menu.Show(instance_, hwnd_, cursor, palette_, scale_);
    tray_menu_open_ = false;
    if (choice == 0) {
        ShowFromTray();
    } else if (choice == 1) {
        ShowSettings();
    } else if (choice == 2) {
        ShowAbout();
    } else if (choice == 3) {
        DestroyWindow(hwnd_);
    }
}

void Application::ShowFromTray() {
    if (edge_collapsed_) {
        RestoreFromEdgeBar();
        return;
    }
    if (!hwnd_) return;
    if (IsWindowVisible(hwnd_) && !in_tray_) {
        ApplyTopMost();
        return;
    }
    in_tray_ = false;
    window_alpha_ = 0.0f;
    PaintNow();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ApplyTopMost();
    StartAnimation();
    ArmEmptyHideTimer();
}

void Application::HideToTray() {
    edge_bar_.Destroy();
    edge_collapsed_ = false;
    if (hwnd_) {
        KillTimer(hwnd_, kTimerEmptyHide);
        ShowWindow(hwnd_, SW_HIDE);
    }
    empty_hide_armed_ = false;
    in_tray_ = true;
    RecordWindowPosition();
}

void Application::ArmEmptyHideTimer() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimerEmptyHide);
    empty_hide_armed_ = false;
    const bool empty = !devices_ || devices_->empty();
    if (!empty || in_tray_ || edge_collapsed_ ||
        settings_.empty_hide_seconds <= 0) {
        return;
    }
    SetTimer(hwnd_, kTimerEmptyHide,
             static_cast<UINT>(settings_.empty_hide_seconds) * 1000, nullptr);
    empty_hide_armed_ = true;
}

// ---------------------------------------------------------------------------
// 边缘悬浮条
// ---------------------------------------------------------------------------

void Application::CheckDropPosition() {
    if (!hwnd_ || edge_collapsed_ || in_tray_) return;
    if (!settings_.edge_collapse) return;

    RECT window = {};
    GetWindowRect(hwnd_, &window);
    const int pad = static_cast<int>(std::lround(view_.kShadowPad * scale_));
    RECT panel = {window.left + pad, window.top + pad, window.right - pad,
                  window.bottom - pad};

    RECT work = WorkAreaForWindow(hwnd_);
    const RECT monitor = MonitorAreaForWindow(hwnd_);

    const bool allow_left = work.left <= monitor.left;
    const bool allow_top = work.top <= monitor.top;
    const bool allow_right = work.right >= monitor.right;
    const bool allow_bottom = work.bottom >= monitor.bottom;

    const int margin = static_cast<int>(std::lround(24.0f * scale_));
    constexpr int kFar = 100000;
    const int left_score =
        allow_left ? panel.left - (work.left + margin) : kFar;
    const int right_score =
        allow_right ? (work.right - margin) - panel.right : kFar;
    const int top_score = allow_top ? panel.top - (work.top + margin) : kFar;
    const int bottom_score =
        allow_bottom ? (work.bottom - margin) - panel.bottom : kFar;

    const int best = std::min(std::min(left_score, right_score),
                              std::min(top_score, bottom_score));
    if (best > 0) return;

    int edge = 1;
    if (best == left_score) {
        edge = 0;
    } else if (best == right_score) {
        edge = 1;
    } else if (best == top_score) {
        edge = 2;
    } else {
        edge = 3;
    }
    CollapseToEdgeBar(edge);
}

void Application::CollapseToEdgeBar(int edge) {
    if (!hwnd_ || edge_collapsed_ || in_tray_) return;
    if (!settings_.edge_collapse) return;

    RECT window = {};
    GetWindowRect(hwnd_, &window);
    const int pad = static_cast<int>(std::lround(view_.kShadowPad * scale_));
    RECT panel = {window.left + pad, window.top + pad, window.right - pad,
                  window.bottom - pad};
    RECT work = WorkAreaForWindow(hwnd_);

    int side = static_cast<int>(std::lround(42.0f * scale_));
    const int max_side =
        std::min(work.right - work.left, work.bottom - work.top) - 16;
    side = std::min(side, max_side);
    side = std::max(side, 32);

    auto clamp_x = [&](int x) {
        if (x + side > work.right) x = work.right - side;
        if (x < work.left) x = work.left;
        return x;
    };
    auto clamp_y = [&](int y) {
        if (y + side > work.bottom) y = work.bottom - side;
        if (y < work.top) y = work.top;
        return y;
    };

    int x = work.right - side;
    int y = clamp_y(panel.top);
    if (edge == 0) {
        x = work.left;
        y = clamp_y(panel.top);
    } else if (edge == 1) {
        x = work.right - side;
        y = clamp_y(panel.top);
    } else if (edge == 2) {
        x = clamp_x(panel.left);
        y = work.top;
    } else {
        x = clamp_x(panel.left);
        y = work.bottom - side;
    }

    edge_side_ = edge;
    edge_collapsed_ = true;
    KillTimer(hwnd_, kTimerEmptyHide);
    empty_hide_armed_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    edge_bar_.Show(instance_, hwnd_, icon_edge_, x, y, side, palette_.dark);
}

void Application::RestoreFromEdgeBar() {
    if (!edge_collapsed_ || !hwnd_) return;
    edge_bar_.Hide();
    edge_bar_.Destroy();
    edge_collapsed_ = false;

    view_.Layout(panel_height_, scale_);
    RECT window = {};
    GetWindowRect(hwnd_, &window);
    RECT work = WorkAreaForWindow(hwnd_);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    int x = window.left;
    int y = window.top;
    if (x + width > work.right) x = work.right - width;
    if (x < work.left) x = work.left;
    if (y + height > work.bottom) y = work.bottom - height;
    if (y < work.top) y = work.top;
    restore_x_ = x;
    restore_y_ = y;

    switch (edge_side_) {
        case 0:
            slide_x_ = -72.0f;
            slide_y_ = 0.0f;
            break;
        case 2:
            slide_x_ = 0.0f;
            slide_y_ = -72.0f;
            break;
        case 3:
            slide_x_ = 0.0f;
            slide_y_ = 72.0f;
            break;
        default:
            slide_x_ = 72.0f;
            slide_y_ = 0.0f;
            break;
    }

    SetWindowPos(hwnd_, nullptr, restore_x_ + static_cast<int>(slide_x_),
                 restore_y_ + static_cast<int>(slide_y_), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    in_tray_ = false;
    window_alpha_ = 1.0f;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ApplyTopMost();
    PaintNow();
    StartAnimation();
    if (settings_.position_mode == 1) {
        settings_.last_x = restore_x_;
        settings_.last_y = restore_y_;
        settings_.has_last_position = true;
    }
    ArmEmptyHideTimer();
}

// ---------------------------------------------------------------------------
// 位置与提示
// ---------------------------------------------------------------------------

POINT Application::DefaultWindowOrigin(int width_px, int height_px) const {
    RECT work = {};
    if (settings_.position_mode == 1 && settings_.has_last_position) {
        const POINT saved = {settings_.last_x, settings_.last_y};
        work = WorkAreaForPoint(saved);
    } else {
        work = WorkAreaForWindow(hwnd_);
    }
    const int margin = static_cast<int>(std::lround(18.0f * scale_));

    if (settings_.position_mode == 1 && settings_.has_last_position) {
        POINT point = {settings_.last_x, settings_.last_y};
        if (point.x > work.right - 64) point.x = work.right - 64;
        if (point.y > work.bottom - 64) point.y = work.bottom - 64;
        if (point.x + width_px < work.left + 64) {
            point.x = work.left + 64 - width_px;
        }
        if (point.y + height_px < work.top + 64) {
            point.y = work.top + 64 - height_px;
        }
        return point;
    }

    POINT point = {};
    switch (settings_.corner) {
        case 0:
            point = {work.left + margin, work.top + margin};
            break;
        case 1:
            point = {work.left + margin, work.bottom - height_px - margin};
            break;
        case 2:
            point = {work.right - width_px - margin, work.top + margin};
            break;
        default:
            point = {work.right - width_px - margin,
                     work.bottom - height_px - margin};
            break;
    }
    return point;
}

void Application::ApplyWindowPosition() {
    if (!hwnd_) return;

    view_.Layout(panel_height_, scale_);
    const int width_px = static_cast<int>(
        std::lround(view_.layout().window_w * scale_));
    const int height_px = static_cast<int>(
        std::lround(view_.layout().window_h * scale_));
    if (width_px <= 0 || height_px <= 0) return;

    const POINT origin = DefaultWindowOrigin(width_px, height_px);
    slide_x_ = 0.0f;
    slide_y_ = 0.0f;
    restore_x_ = origin.x;
    restore_y_ = origin.y;
    canvas_.Resize(width_px, height_px);
    SetWindowPos(hwnd_, nullptr, origin.x, origin.y, width_px, height_px,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    if (settings_.position_mode == 1) {
        settings_.last_x = origin.x;
        settings_.last_y = origin.y;
        settings_.has_last_position = true;
    }
}

void Application::RecordWindowPosition() {
    if (!hwnd_ || settings_.position_mode != 1) return;

    if (std::fabs(slide_x_) > 0.5f || std::fabs(slide_y_) > 0.5f) {
        // 滑入动画期间窗口还带着位移，记录目标位置而不是中间帧。
        settings_.last_x = restore_x_;
        settings_.last_y = restore_y_;
        settings_.has_last_position = true;
        return;
    }

    RECT window = {};
    GetWindowRect(hwnd_, &window);
    settings_.last_x = window.left;
    settings_.last_y = window.top;
    settings_.has_last_position = true;
}

void Application::SaveSettings() {
    if (settings_.position_mode == 1) RecordWindowPosition();
    store_.Save(settings_);
}

void Application::ShowToast(std::wstring text, bool ok) {
    view_.ShowToast(std::move(text), ok);
    if (hwnd_) SetTimer(hwnd_, kTimerToast, 4200, nullptr);
    StartAnimation();
}

// ---------------------------------------------------------------------------
// 定时器
// ---------------------------------------------------------------------------

void Application::OnTimer(UINT_PTR timer) {
    switch (timer) {
        case kTimerAnim:
            TickAnimation();
            break;
        case kTimerScan:
            KillTimer(hwnd_, kTimerScan);
            StartScan();
            break;
        case kTimerPoll:
            StartScan();
            break;
        case kTimerOccupantsRefresh:
            CheckOccupantsRefresh();
            break;
        case kTimerUpdateAuto:
            KillTimer(hwnd_, kTimerUpdateAuto);
            if (settings_.auto_check_updates) StartUpdateCheck(true);
            break;
        case kTimerEmptyHide:
            KillTimer(hwnd_, kTimerEmptyHide);
            empty_hide_armed_ = false;
            if (devices_->empty() && !in_tray_ && !edge_collapsed_) {
                HideToTray();
            }
            break;
        case kTimerTray:
            KillTimer(hwnd_, kTimerTray);
            AddTrayIcon();
            break;
        case kTimerTrayHealth: {
            if (!tray_registered_ || !hwnd_) {
                KillTimer(hwnd_, kTimerTrayHealth);
                break;
            }
            NOTIFYICONIDENTIFIER identifier = {};
            identifier.cbSize = sizeof(identifier);
            identifier.hWnd = hwnd_;
            identifier.uID = 1;
            RECT rect = {};
            if (FAILED(Shell_NotifyIconGetRect(&identifier, &rect))) {
                // 资源管理器重启或图标被系统丢弃：重新注册。
                tray_registered_ = false;
                KillTimer(hwnd_, kTimerTrayHealth);
                AddTrayIcon();
            }
            break;
        }
        case kTimerToast:
            KillTimer(hwnd_, kTimerToast);
            view_.HideToast();
            StartAnimation();
            break;
        case kTimerAction:
            KillTimer(hwnd_, kTimerAction);
            ++action_generation_;
            action_busy_ = false;
            ejecting_disk_ = 0;
            busy_scanning_ = false;
            busy_releasing_ = false;
            if (dialog_ == ui::DialogKind::Busy) {
                CloseDialog();
            } else {
                RebuildModel();
                StartAnimation();
                PaintNow();
            }
            ShowToast(L"操作超时，已取消。", false);
            RequestScan(300);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

LRESULT CALLBACK Application::WindowProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    Application* self = reinterpret_cast<Application*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT Application::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
    if (taskbar_created_message_ != 0 && message == taskbar_created_message_) {
        AddTrayIcon();
        return 0;
    }

    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            PaintNow();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST: {
            POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            return view_.OverPanel(point.x / scale_, point.y / scale_)
                       ? HTCLIENT
                       : HTTRANSPARENT;
        }

        case WM_LBUTTONDOWN: {
            if (in_tray_) return 0;
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            const ui::ViewAction action = view_.OnMouseDown(x, y);
            if (action.kind != ui::ActionKind::None) OnAction(action);
            StartAnimation();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!tracking_leave_) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                tracking_leave_ = true;
            }
            if (empty_hide_armed_ && devices_->empty()) ArmEmptyHideTimer();
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            const ui::ViewAction action = view_.OnMouseMove(x, y);
            if (action.kind != ui::ActionKind::None) OnAction(action);
            // 只有真的需要过渡动画时才启动定时器：拖动窗口时画面不变，
            // 每帧整窗重绘会拖慢窗口跟随，还会和拖动抢消息队列。
            if (view_.Animating()) StartAnimation();
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            view_.OnMouseLeave();
            StartAnimation();
            return 0;

        case WM_LBUTTONUP: {
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            const ui::ViewAction action = view_.OnMouseUp(x, y);
            if (action.kind != ui::ActionKind::None) OnAction(action);
            StartAnimation();
            return 0;
        }

        case WM_CAPTURECHANGED:
            view_.CancelInteraction();
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                POINT point = {};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                const bool interactive = view_.OverInteractive(
                    point.x / scale_, point.y / scale_);
                SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND
                                                           : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_MOUSEWHEEL: {
            const float delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0f;
            if (view_.OnWheel(delta)) StartAnimation();
            return 0;
        }

        case WM_TIMER:
            OnTimer(static_cast<UINT_PTR>(wparam));
            return 0;

        case WM_DEVICECHANGE:
            if (wparam == DBT_DEVICEARRIVAL ||
                wparam == DBT_DEVICEREMOVECOMPLETE ||
                wparam == DBT_DEVNODES_CHANGED) {
                RequestScan(300);
                return TRUE;
            }
            break;

        case kMessageShellNotify:
            RequestScan(300);
            return 0;

        case kMessageScanDone: {
            auto payload = TakePayload<ScanPayload>(lparam);
            scan_in_flight_ = false;
            if (payload) ApplyDevices(std::move(payload->devices));
            if (scan_pending_) {
                scan_pending_ = false;
                StartScan();
            }
            return 0;
        }

        case kMessageEjectDone: {
            auto payload = TakePayload<EjectPayload>(lparam);
            if (!payload) return 0;
            if (payload->generation != action_generation_) return 0;

            KillTimer(hwnd_, kTimerAction);
            action_busy_ = false;
            ejecting_disk_ = 0;
            busy_scanning_ = false;
            busy_releasing_ = false;
            RebuildModel();

            const EjectResult& result = payload->result;
            if (result.ok()) {
                CloseDialog();
                ShowToast(result.message.empty()
                              ? L"已安全弹出设备，现在可以拔下设备了。"
                              : result.message,
                          true);
                RequestScan(350);
                return 0;
            }

            if (result.status == EjectStatus::Busy) {
                if (payload->from_release && payload->released == 0) {
                    CloseDialog();
                    ShowToast(result.message, false);
                    RequestScan(400);
                    return 0;
                }
                if (payload->from_release && payload->released > 0) {
                    StartOccupantScan();
                    ShowToast(result.message, false);
                    return 0;
                }
                ShowBusyDialog(payload->disk_number, payload->title,
                               payload->targets);
                return 0;
            }

            if (result.status == EjectStatus::AccessDenied) {
                std::vector<wchar_t> letters =
                    LettersFromTargets(payload->targets);
                if (!IsProcessElevated() && !letters.empty()) {
                    ShowElevationPrompt(
                        AdminAction::Eject,
                        L"将解除「" + payload->title + L"」的占用并安全弹出。",
                        payload->disk_number, payload->title, letters, L"", 0);
                } else {
                    CloseDialog();
                    ShowToast(
                        L"即使以管理员身份运行，仍无法弹出该设备，请检查占用"
                        L"后重试。",
                        false);
                }
                RequestScan(400);
                return 0;
            }

            CloseDialog();
            ShowToast(result.message.empty() ? L"弹出失败，请稍后重试。"
                                             : result.message,
                      false);
            RequestScan(400);
            return 0;
        }

        case kMessageOccupantsDone: {
            auto payload = TakePayload<OccupantsPayload>(lparam);
            if (!payload) return 0;
            if (payload->generation != action_generation_) return 0;

            // 后台复检：保留“扫描中”状态不动，只在列表真的变化时刷新一次，
            // 且扫描失败时不清空现有条目，避免闪烁和误清仍在运行的进程。
            if (payload->refresh) {
                if (dialog_ != ui::DialogKind::Busy || busy_releasing_) return 0;
                if (!payload->complete) return 0;
                if (OccupantPids(*occupants_) ==
                    OccupantPids(*payload->occupants)) {
                    return 0;
                }
                occupants_ = std::move(payload->occupants);
                RebuildModel();
                StartAnimation();
                PaintNow();
                SyncOccupantsRefreshTimer();
                return 0;
            }

            KillTimer(hwnd_, kTimerAction);
            action_busy_ = false;
            busy_scanning_ = false;
            if (dialog_ == ui::DialogKind::Busy) {
                occupants_ = std::move(payload->occupants);
                RebuildModel();
                StartAnimation();
                PaintNow();
                SyncOccupantsRefreshTimer();
            }
            return 0;
        }

        case kMessageUpdateDone: {
            auto payload = TakePayload<UpdatePayload>(lparam);
            if (!payload) return 0;
            if (payload->generation != update_generation_) return 0;
            update_busy_ = false;
            const UpdateCheckResult& result = payload->result;
            USB_LOG_INFO(L"update", L"更新检查结果：status=%d latest=%s",
                         static_cast<int>(result.status),
                         result.latest_version.c_str());

            if (kDevBuild) {
                // 开发版：每次检查都必须让用户看到结果（含启动自动检查）。
                if (!about_window_.IsOpen()) ShowAbout();
                about_window_.SetUpdateResult(result);
                return 0;
            }

            if (about_window_.IsOpen()) about_window_.SetUpdateResult(result);
            // 启动时的静默检查只在发现新版本时提示；失败与已是最新都不打扰。
            if (update_silent_ &&
                result.status == UpdateCheckResult::Status::UpdateAvailable) {
                ShowUpdateBalloon(result.latest_version);
            }
            return 0;
        }

        case kMessageHelperDone: {
            auto payload = TakePayload<HelperPayload>(lparam);
            if (!payload) return 0;
            if (payload->generation != action_generation_) return 0;

            KillTimer(hwnd_, kTimerAction);
            action_busy_ = false;
            const HelperOutcome& outcome = payload->outcome;
            USB_LOG_INFO(L"app",
                         L"提权结果：op=%d status=%d letter=%c cancelled=%d "
                         L"message=%s",
                         static_cast<int>(payload->operation), outcome.status,
                         outcome.letter ? outcome.letter : L'-',
                         outcome.cancelled ? 1 : 0, outcome.message.c_str());

            if (outcome.cancelled) {
                ShowToast(outcome.message.empty()
                              ? L"未获得管理员权限，操作已取消。"
                              : outcome.message,
                          false);
                RequestScan(300);
                return 0;
            }

            if (payload->operation == HelperOperation::Open) {
                if (outcome.status == 0) {
                    ShowToast(L"已用管理员权限打开 " + payload->root +
                                  L"（资源管理器为管理员窗口）。",
                              true);
                } else {
                    ShowToast(outcome.message.empty()
                                  ? L"管理员模式下打开分区失败。"
                                  : outcome.message,
                              false);
                }
                return 0;
            }

            if (payload->operation == HelperOperation::Mount) {
                if (outcome.status ==
                        static_cast<int>(storage::MountStatus::Ok) &&
                    outcome.letter != 0) {
                    ApplyElevatedMount(payload->volume_path, outcome.letter);
                    ShowToast(L"已将分区挂载为 " +
                                  std::wstring(1, outcome.letter) +
                                  L":（等同于磁盘管理中分配盘符）",
                              true);
                    RequestScan(450);
                } else {
                    const std::wstring feedback =
                        outcome.status ==
                                static_cast<int>(
                                    storage::MountStatus::AccessDenied)
                            ? L"即使以管理员身份运行，仍无法挂载该分区，"
                              L"请检查系统策略后重试。"
                            : (outcome.message.empty()
                                   ? L"管理员模式下挂载失败。"
                                   : outcome.message);
                    ShowToast(feedback, false);
                    RequestScan(400);
                }
                return 0;
            }

            if (payload->operation == HelperOperation::Unmount) {
                if (outcome.status ==
                    static_cast<int>(storage::MountStatus::Ok)) {
                    ApplyElevatedUnmount(payload->letter);
                    ShowToast(L"已取消挂载 " +
                                  std::wstring(1, payload->letter) + L":",
                              true);
                    RequestScan(400);
                } else {
                    const std::wstring feedback =
                        outcome.status ==
                                static_cast<int>(
                                    storage::MountStatus::AccessDenied)
                            ? L"即使以管理员身份运行，仍无法取消挂载该分区，"
                              L"请稍后重试。"
                            : (outcome.message.empty()
                                   ? L"管理员模式下取消挂载失败。"
                                   : outcome.message);
                    ShowToast(feedback, false);
                    RequestScan(400);
                }
                return 0;
            }

            if (outcome.status == static_cast<int>(EjectStatus::Ok)) {
                ShowToast(outcome.message.empty()
                              ? L"已安全弹出设备，现在可以拔下设备了。"
                              : outcome.message,
                          true);
                RequestScan(350);
                return 0;
            }
            if (outcome.status == static_cast<int>(EjectStatus::Busy)) {
                std::vector<EjectTarget> targets =
                    TargetsFromLetters(payload->letters);
                ShowBusyDialog(payload->disk_number, payload->title, targets);
                return 0;
            }
            if (outcome.status == static_cast<int>(EjectStatus::AccessDenied)) {
                ShowToast(
                    L"即使以管理员身份运行，仍无法移除该设备，请检查占用后"
                    L"重试。",
                    false);
                RequestScan(400);
                return 0;
            }
            if (outcome.status == static_cast<int>(EjectStatus::NotFound)) {
                ShowToast(L"设备已不在位，可能已被拔出。", true);
                RequestScan(300);
                return 0;
            }
            ShowToast(outcome.message.empty() ? L"管理员模式弹出失败。"
                                              : outcome.message,
                      false);
            RequestScan(400);
            return 0;
        }

        case kMessageTray: {
            const UINT event = LOWORD(lparam);
            if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK ||
                event == NIN_SELECT) {
                ShowFromTray();
            } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;
        }

        case kMessageShowMain:
            ShowFromTray();
            return 0;

        case ui::kMessageEdgeBarClicked:
            RestoreFromEdgeBar();
            return 0;

        case WM_MOVE:
            return 0;

        case WM_DISPLAYCHANGE:
            UpdatePanelTarget();
            RecalculateWindow();
            StartAnimation();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam) ? static_cast<float>(HIWORD(wparam))
                                  : SystemDpi();
            scale_ = dpi_ / 96.0f;
            RebuildModel();
            panel_target_ = view_.preferred_panel_height(MaxPanelHeight());
            panel_height_ = panel_target_;
            RecalculateWindow();
            if (RECT* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, 0,
                             0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            PaintNow();
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            UpdateTheme();
            return 0;

        case WM_CLOSE:
            RequestClose();
            return 0;

        case WM_QUERYENDSESSION:
            SaveSettings();
            UnmountManagedVolumes(PartitionKind::Normal, true);
            return TRUE;

        case WM_ENDSESSION:
            if (wparam) {
                SaveSettings();
                UnmountManagedVolumes(PartitionKind::Normal, true);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd_, kTimerAnim);
            KillTimer(hwnd_, kTimerScan);
            KillTimer(hwnd_, kTimerPoll);
            KillTimer(hwnd_, kTimerEmptyHide);
            KillTimer(hwnd_, kTimerTray);
            KillTimer(hwnd_, kTimerTrayHealth);
            KillTimer(hwnd_, kTimerToast);
            KillTimer(hwnd_, kTimerAction);
            KillTimer(hwnd_, kTimerUpdateAuto);
            SaveSettings();
            UnmountManagedVolumes(PartitionKind::Normal, true);
            RemoveTrayIcon();
            if (shell_notify_id_ != 0) {
                SHChangeNotifyDeregister(shell_notify_id_);
                shell_notify_id_ = 0;
            }
            settings_window_.Close();
            about_window_.Close();
            edge_bar_.Destroy();
            edge_collapsed_ = false;
            {
                std::lock_guard<std::mutex> lock(post_target_->mutex);
                post_target_->hwnd = nullptr;
            }
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::app
