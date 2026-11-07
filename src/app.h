#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "canvas.h"
#include "com_ptr.h"
#include "device.h"
#include "eject.h"
#include "occupant.h"
#include "settings.h"
#include "theme.h"

#define WM_APP_EJECT_DONE (WM_APP + 1)
#define WM_APP_TRAY (WM_APP + 2)
#define WM_APP_SHELL_NOTIFY (WM_APP + 3)
#define WM_APP_DEVICES_READY (WM_APP + 4)
#define WM_APP_SHOW_MAIN (WM_APP + 5)
#define WM_APP_OCCUPANTS_READY (WM_APP + 6)

// 后台刷新线程与主线程之间传递的枚举结果（线程结束时自动释放）
struct RefreshState {
    std::mutex mutex;
    std::vector<DeviceInfo> devices;
    bool ready = false;
    std::atomic<bool> running{false};
    std::atomic<bool> cancelled{false};
    HWND hwnd = nullptr;
};

struct Fonts {
    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* subtitle = nullptr;
    IDWriteTextFormat* cardName = nullptr;
    IDWriteTextFormat* partName = nullptr;
    IDWriteTextFormat* meta = nullptr;
    IDWriteTextFormat* pill = nullptr;
    IDWriteTextFormat* button = nullptr;
    IDWriteTextFormat* emptyTitle = nullptr;
    IDWriteTextFormat* emptyHint = nullptr;
    IDWriteTextFormat* toast = nullptr;
    IDWriteTextFormat* dialogTitle = nullptr;
    IDWriteTextFormat* dialogRow = nullptr;
    void Release();
};

struct PartitionLayout {
    D2D1_RECT_F row = {};
    D2D1_RECT_F openBtn = {};
    D2D1_RECT_F pill = {};
};

struct CardLayout {
    D2D1_RECT_F card = {};
    D2D1_RECT_F ejectBtn = {};
    std::vector<PartitionLayout> parts;
};

struct UiLayout {
    float windowW = 0.0f;
    float windowH = 0.0f;
    D2D1_RECT_F panel = {};
    D2D1_RECT_F closeBtn = {};
    D2D1_RECT_F contentClip = {};
    float contentTop = 0.0f;
    std::vector<CardLayout> cards;
    bool scrolling = false;
    float scrollMax = 0.0f;
    D2D1_RECT_F scrollBar = {}; // 滚动条命中区域（仅 scrolling 时有效）

    // 退出确认弹层
    bool confirmVisible = false;
    D2D1_RECT_F dlgCard = {};
    D2D1_RECT_F dlgCheck = {};
    D2D1_RECT_F dlgCancel = {};
    D2D1_RECT_F dlgOk = {};

    // 设备被占用弹层（进程列表 + 解除占用）
    bool busyVisible = false;
    D2D1_RECT_F busyCard = {};
    D2D1_RECT_F busyTitle = {};
    D2D1_RECT_F busyList = {};
    D2D1_RECT_F busyCancel = {};
    D2D1_RECT_F busyRelease = {};
    float busyScrollMax = 0.0f;
};

enum class HitKind {
    None,
    Close,
    Open,
    Eject,
    ConfirmOk,
    ConfirmCancel,
    ConfirmCheck,
    BusyCancel,
    BusyRelease,
    ScrollBar
};

struct HitResult {
    HitKind kind = HitKind::None;
    int card = -1;
    int part = -1;
};

class App {
public:
    bool Init(HINSTANCE hinst, bool startHidden = false);
    int Run();

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // 渲染模块只读访问
    const std::vector<DeviceInfo>& Devices() const { return devices_; }
    const Palette& Colors() const { return palette_; }
    const Fonts& Text() const { return fonts_; }
    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    const UiLayout& Layout() const { return layout_; }
    float ScrollY() const { return scrollY_; }
    float ToastOpacity() const { return toastOpacity_; }
    const std::wstring& ToastText() const { return toastText_; }
    bool ToastSuccess() const { return toastOk_; }
    float HoverClose() const { return hoverClose_; }
    bool ConfirmVisible() const { return confirmClose_; }
    bool ConfirmDontAsk() const { return confirmDontAsk_; }
    bool BusyVisible() const { return busyDialog_; }
    const std::wstring& BusyTitle() const { return busyTitle_; }
    const std::vector<VolumeOccupant>& BusyOccupants() const {
        return busyOccupants_;
    }
    float BusyScroll() const { return busyScroll_; }
    bool BusyScanning() const { return busyScanning_; }
    // 滚动条悬浮/拖动时的加粗进度（0 = 细，1 = 粗）
    float ScrollGrow() const { return scrollGrow_; }

private:
    void RefreshDevices();
    void ApplyDevices(std::vector<DeviceInfo> fresh);
    void OnDevicesReady();
    void ArmEmptyHideTimer();
    void RecalcLayout();
    void ComputeTargetPanelHeight();
    void ApplyWindowSize();
    void RequestPaint();
    void PaintNow();
    void StartAnim();
    bool AnimTick();
    void SetHover(HitResult hit);
    HitResult HitTest(float x, float y) const;
    float ScrollThumbHeight() const;
    D2D1_RECT_F ScrollThumbRect() const;
    void ActivateHit(HitResult hit);
    void StartEject(int cardIndex);
    void ShowToast(const std::wstring& text, bool ok);
    void ShowBusyDialog(unsigned int disk, const std::wstring& title,
                        std::vector<EjectVolumeTarget> volumes,
                        std::vector<VolumeOccupant> occupants);
    void CloseBusyDialog();
    void ReleaseOccupantsAndEject();
    void StartOccupantScan();
    void CheckDropPosition();
    void UpdateTheme();

    void ApplyTopMost();
    void ApplyRefreshTimer();
    POINT DefaultWindowOrigin(int widthPx, int heightPx) const;
    void RecordWindowPosition();
    void ShowSettings();
    void ApplySettings();
    void CollapseToEdgeBar(int edge);
    void RestoreFromEdgeBar();
    void DestroyEdgeBar();

    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void HideToTray();
    void ShowFromTray();

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK StaticEdgeBarProc(HWND hwnd, UINT msg, WPARAM wp,
                                              LPARAM lp);

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    Fonts fonts_ = {};
    Canvas canvas_;
    Palette palette_ = MakePalette(false);

    std::vector<DeviceInfo> devices_;
    UiLayout layout_ = {};

    float dpi_ = 96.0f;
    float scale_ = 1.0f;

    float panelH_ = 0.0f;
    float targetPanelH_ = 0.0f;
    float scrollY_ = 0.0f;
    float scrollTarget_ = 0.0f;
    float windowAlpha_ = 0.0f;
    float toastOpacity_ = 0.0f;
    float toastTarget_ = 0.0f;
    std::wstring toastText_;
    bool toastOk_ = true;
    bool toastVisible_ = false;

    float hoverClose_ = 0.0f;
    float hoverCloseTarget_ = 0.0f;

    // 退出确认弹层
    bool confirmClose_ = false;
    bool confirmDontAsk_ = false;

    // 设备被占用弹层
    bool busyDialog_ = false;
    std::wstring busyTitle_;
    unsigned int busyDisk_ = 0;
    std::vector<EjectVolumeTarget> busyVolumes_;
    std::vector<VolumeOccupant> busyOccupants_;
    float busyScroll_ = 0.0f;
    float busyScrollTarget_ = 0.0f;
    bool busyScanning_ = false;
    std::mutex occupantMutex_;
    std::vector<VolumeOccupant> occupantScanResult_;
    bool occupantScanReady_ = false;

    // 边缘悬浮条
    HWND edgeBar_ = nullptr;
    bool collapsed_ = false;
    int edgeSide_ = 1;        // 悬浮条所在边缘：0 = 左，1 = 右，2 = 上，3 = 下
    float slideOffset_ = 0.0f;  // 恢复时水平滑入偏移（物理像素）
    float slideOffsetY_ = 0.0f; // 恢复时垂直滑入偏移（物理像素）
    int restoreX_ = 0;
    int restoreY_ = 0;

    HitResult hovered_ = {};
    HitResult pressed_ = {};
    // 滚动条交互状态
    bool scrollHover_ = false;
    bool scrollDragging_ = false;
    float scrollGrow_ = 0.0f;
    float scrollDragStartY_ = 0.0f;
    float scrollDragStartTarget_ = 0.0f;
    bool dragging_ = false;
    POINT dragCursorStart_ = {};
    RECT dragWindowStart_ = {};
    bool trackingLeave_ = false;
    bool inTray_ = false;
    bool animating_ = false;

    HICON iconSmall_ = nullptr;
    HICON iconLarge_ = nullptr;
    HICON iconEdge_ = nullptr; // 边缘悬浮条使用的大尺寸图标
    NOTIFYICONDATAW trayData_ = {};

    Settings settings_ = {};
    ULONG shellNotifyId_ = 0;

    std::shared_ptr<RefreshState> refreshState_;
    bool refreshQueued_ = false;
    bool emptyHideArmed_ = false;

    bool ejectBusy_ = false;
    std::thread ejectThread_;
    std::mutex ejectMutex_;
    bool ejectResultPending_ = false;
    bool ejectResultOk_ = false;
    bool ejectResultOccupied_ = false;
    unsigned int ejectResultDisk_ = 0;
    std::wstring ejectResultMessage_;
    std::wstring ejectResultTitle_;
    std::vector<EjectVolumeTarget> ejectResultVolumes_;

    // 弹出失败时先等设备枚举刷新再确认：磁盘节点常会延迟消失
    bool ejectVerifyPending_ = false;
    unsigned int ejectVerifyDisk_ = 0;
    std::wstring ejectVerifyTitle_;
    std::wstring ejectVerifyMessage_;
};
