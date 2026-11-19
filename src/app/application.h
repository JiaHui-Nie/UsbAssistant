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
    void ApplyRefreshTimer();
    void ApplySettings(const Settings& settings);
    void ShowSettings();
    void ShowAbout();
    void StartUpdateCheck(bool silent);
    void ShowUpdateBalloon(const std::wstring& latest_version);

    // 设备
    DeviceFilter CurrentFilter() const;
    void RequestScan(int delay_ms);
    void StartScan();
    void ApplyDevices(std::shared_ptr<const std::vector<Device>> devices);

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
    void ShowFromTray();
    void HideToTray();
    void ArmEmptyHideTimer();

    // 边缘悬浮条
    void CheckDropPosition();
    void CollapseToEdgeBar(int edge);
    void RestoreFromEdgeBar();

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
    bool com_initialized_ = false;
    bool start_hidden_ = false;
    bool in_tray_ = false;
    bool edge_collapsed_ = false;

    SettingsStore store_;
    Settings settings_;
    ui::Palette palette_ = ui::MakePalette(false);
    ui::Fonts fonts_;
    ui::Canvas canvas_;
    ui::MainView view_;
    ui::SettingsWindow settings_window_;
    ui::AboutWindow about_window_;
    ui::EdgeBar edge_bar_;

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    HICON icon_small_ = nullptr;
    HICON icon_large_ = nullptr;
    HICON icon_edge_ = nullptr;
    NOTIFYICONDATAW tray_data_ = {};
    bool tray_registered_ = false;
    ULONG shell_notify_id_ = 0;

    std::shared_ptr<const std::vector<Device>> devices_;
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
    Worker update_worker_;
    std::shared_ptr<PostTarget> post_target_ = std::make_shared<PostTarget>();
    bool scan_in_flight_ = false;
    bool scan_pending_ = false;
    bool action_busy_ = false;
    std::uint64_t action_generation_ = 0;

    // 更新检查：结果回传后只在 generation 匹配时更新界面。
    bool update_busy_ = false;
    bool update_silent_ = false;
    std::uint64_t update_generation_ = 0;

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
    // 托盘通知可能重复投递（WM_RBUTTONUP 与 WM_CONTEXTMENU），
    // 菜单显示期间忽略后续通知，避免叠出多个菜单窗口。
    bool tray_menu_open_ = false;
};

}  // namespace usb::app
