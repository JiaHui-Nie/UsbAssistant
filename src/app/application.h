// 应用控制器：拥有主窗口、托盘、设置、边缘悬浮条和后台任务。
// 只在这里做业务编排；设备/弹出/占用等细节都在 core 中。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/post_target.h"
#include "app/worker.h"
#include "core/models.h"
#include "core/settings.h"
#include "ui/about_window.h"
#include "ui/canvas.h"
#include "ui/edge_bar.h"
#include "ui/main_view.h"
#include "ui/message_dialog.h"
#include "ui/settings_window.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/tray_menu.h"

namespace usb::app {

class Application {
public:
    Application();
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool Initialize(HINSTANCE instance, bool start_hidden);
    int Run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    // 窗口与绘制
    void RebuildModel();
    void UpdatePanelTarget();
    void RecalculateWindow();
    void PaintNow();
    void StartAnimation();
    void TickAnimation();
    void OnTimer(UINT_PTR timer);
    void UpdateTheme();
    void ApplyTopMost();
    // 把主窗口压到最前。activate=true 用于用户主动唤出（托盘/边缘条点
    // 击），会尝试取得前台焦点；false 用于设备插入等被动场景，只调整
    // z 序，不抢占用户当前输入焦点。
    void RaiseMainWindow(bool activate);
    // 取得前台激活权限：附加到当前前台窗口的输入线程后调用
    // SetForegroundWindow，是绕开前台锁的常规做法。
    void ActivateForeground();
    void ApplyRefreshTimer();
    void ApplySettings(const Settings& settings);
    void ShowSettings();
    // 校验并注册全局快捷键；成功返回空字符串，失败返回给用户看的原因。
    std::wstring ChangeHotkey(int mods, int vk);
    void SyncHotkey(bool startup);
    void ShowAbout();
    // 升级器启动方式：Manual 始终显示窗口，AutoCheck 只在新版本时显示，
    // AutoSilent 完全不显示窗口，Cleanup 只清理上一次留下的旧版升级器。
    enum class UpdateLaunch { Manual, AutoCheck, AutoSilent, Cleanup };
    void LaunchUpdater(UpdateLaunch launch);
    // 按“自动更新开关 + 频率 + 上次检查时间”判断本次要不要检查。
    // startup=true 时即使不检查也要运行一次升级器：它负责删掉上一次
    // 自我替换留下的 .old 文件。
    void RunScheduledUpdateCheck(bool startup);
    void ShowUpdaterLaunchError();

    // 设备
    DeviceFilter CurrentFilter() const;
    void RequestScan(int delay_ms);
    void StartScan();
    void ApplyDevices(std::shared_ptr<const std::vector<Device>> devices);
    void BringWindowToFrontForDevice();

    // 交互
    void OnAction(const ui::ViewAction& action);
    void OpenPartition(std::size_t device_index, std::size_t partition_index);
    void MountPartition(std::size_t device_index, std::size_t partition_index);
    void UnmountPartition(std::size_t device_index,
                          std::size_t partition_index);
    void UnmountManagedVolumes(PartitionKind kind, bool all,
                               bool notify = false);
    void PruneMountedVolumes();
    void RequestClose();
    void ConfirmExit();
    void CloseDialog();

    // 弹出流程
    enum class AdminAction { None, Eject, Mount, Unmount, Open };
    void StartEject(std::size_t device_index);
    void StartEjectByDisk(std::uint32_t disk_number);
    void ShowBusyDialog(std::uint32_t disk_number, const std::wstring& title,
                        const std::vector<EjectTarget>& targets);
    void StartOccupantScan();
    void StartOccupantRefresh();
    void CheckOccupantsRefresh();
    void SyncOccupantsRefreshTimer();
    void ReleaseOccupantsAndRetry();
    void ShowElevationPrompt(AdminAction action, const std::wstring& detail,
                             std::uint32_t disk_number,
                             const std::wstring& title,
                             const std::vector<wchar_t>& letters,
                             const std::wstring& volume_path, wchar_t letter,
                             const std::wstring& root = L"");
    void RunElevated();
    void ApplyElevatedMount(const std::wstring& volume_path, wchar_t letter);
    void ApplyElevatedUnmount(wchar_t letter);

    // 托盘 / 隐藏
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void ShowFromTray(bool activate = true);
    void HideToTray();
    void ArmEmptyHideTimer();

    // 边缘悬浮条
    void CheckDropPosition();
    void CollapseToEdgeBar(int edge);
    void RestoreFromEdgeBar(bool activate = true);

    // 位置
    POINT DefaultWindowOrigin(int width_px, int height_px) const;
    void ApplyWindowPosition();
    void RecordWindowPosition();
    void SaveSettings();
    void ShowToast(std::wstring text, bool ok);

    float MaxPanelHeight() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_message_ = 0;
    UINT exit_request_message_ = 0;
    bool com_initialized_ = false;
    bool start_hidden_ = false;
    bool in_tray_ = false;
    bool edge_collapsed_ = false;

    SettingsStore store_;
    Settings settings_;
    // 当前实际注册成功的全局快捷键，用于避免重复注册和失败回滚。
    int hotkey_mods_ = 0;
    int hotkey_vk_ = 0;
    bool hotkey_registered_ = false;
    ui::Palette palette_ = ui::MakePalette(false);
    ui::Fonts fonts_;
    ui::Canvas canvas_;
    ui::MainView view_;
    ui::SettingsWindow settings_window_;
    ui::AboutWindow about_window_;
    ui::MessageDialog update_error_dialog_;
    ui::EdgeBar edge_bar_;

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    HICON icon_small_ = nullptr;
    HICON icon_large_ = nullptr;
    NOTIFYICONDATAW tray_data_ = {};
    bool tray_registered_ = false;
    ULONG shell_notify_id_ = 0;

    std::shared_ptr<const std::vector<Device>> devices_;
    bool has_device_snapshot_ = false;
    std::shared_ptr<const std::vector<Occupant>> occupants_;
    std::uint32_t ejecting_disk_ = 0;

    // 仅记录本进程分配的盘符，退出或隐藏分区时按此清理。
    struct MountedVolume {
        std::wstring volume_path;
        wchar_t letter = 0;
        PartitionKind kind = PartitionKind::Normal;
    };
    std::vector<MountedVolume> mounted_volumes_;

    ui::DialogKind dialog_ = ui::DialogKind::None;
    std::wstring busy_title_;
    std::uint32_t busy_disk_ = 0;
    std::vector<EjectTarget> busy_targets_;
    bool busy_scanning_ = false;
    bool busy_releasing_ = false;
    int occupants_refresh_ticks_ = 0;
    AdminAction admin_action_ = AdminAction::None;
    std::wstring admin_detail_;
    std::uint32_t admin_disk_ = 0;
    std::wstring admin_title_;
    std::vector<wchar_t> admin_letters_;
    std::wstring admin_volume_path_;
    wchar_t admin_letter_ = 0;
    std::wstring admin_root_;
    bool confirm_dont_ask_ = false;

    Worker scan_worker_;
    Worker action_worker_;
    std::shared_ptr<PostTarget> post_target_ = std::make_shared<PostTarget>();
    bool scan_in_flight_ = false;
    bool scan_pending_ = false;
    bool action_busy_ = false;
    std::uint64_t action_generation_ = 0;

    float dpi_ = 96.0f;
    float scale_ = 1.0f;
    float panel_height_ = 0.0f;
    float panel_target_ = 0.0f;
    float window_alpha_ = 1.0f;
    float slide_x_ = 0.0f;
    float slide_y_ = 0.0f;
    int restore_x_ = 0;
    int restore_y_ = 0;
    int edge_side_ = 1;
    bool animating_ = false;
    bool tracking_leave_ = false;
    bool empty_hide_armed_ = false;
    // 触摸屏：只跟踪第一个触点，其余触点在抬手前忽略。
    bool touch_contact_active_ = false;
    DWORD touch_contact_id_ = 0;
    // 启动后的第一次自动更新判断是否已经执行过（设置变更时会重置）。
    bool update_startup_done_ = false;
    // 托盘通知可能重复投递（WM_RBUTTONUP 与 WM_CONTEXTMENU），
    // 菜单显示期间忽略后续通知，避免叠出多个菜单窗口。
    bool tray_menu_open_ = false;
};

}  // namespace usb::app
