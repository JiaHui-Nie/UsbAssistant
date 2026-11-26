// 主界面视图：布局、绘制、命中测试、指针/滚轮交互与动画。
// 不持有业务数据，所有内容通过 Model 注入；语义动作通过 ViewAction 返回。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/models.h"
#include "ui/canvas.h"
#include "ui/pointer.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace usb::ui {

enum class DialogKind { None, ConfirmClose, Busy, Admin };

enum class ActionKind {
    None,
    Close,
    ConfirmExit,
    CancelDialog,
    ToggleDontAsk,
    Eject,
    Open,
    Mount,
    Unmount,
    ReleaseOccupants,
    Elevate,
    DragEnded,
};

struct ViewAction {
    ActionKind kind = ActionKind::None;
    std::size_t device = 0;
    std::size_t partition = 0;
};

enum class HitKind {
    None,
    Close,
    Open,
    Eject,
    Mount,
    Unmount,
    ConfirmOk,
    ConfirmCancel,
    ConfirmCheck,
    BusyCancel,
    BusyRelease,
    AdminCancel,
    AdminElevate,
    ScrollBar,
};

struct Hit {
    HitKind kind = HitKind::None;
    int device = -1;
    int partition = -1;
};

struct PartitionLayout {
    D2D1_RECT_F row = {};
    D2D1_RECT_F open_button = {};
    D2D1_RECT_F mount_button = {};
    D2D1_RECT_F unmount_button = {};
    D2D1_RECT_F pill = {};
};

struct CardLayout {
    D2D1_RECT_F card = {};
    D2D1_RECT_F eject_button = {};
    std::vector<PartitionLayout> partitions;
};

struct ViewLayout {
    float window_w = 0.0f;
    float window_h = 0.0f;
    D2D1_RECT_F panel = {};
    D2D1_RECT_F close_button = {};
    D2D1_RECT_F content_clip = {};
    float content_top = 0.0f;
    std::vector<CardLayout> cards;
    bool scrolling = false;
    float scroll_max = 0.0f;
    D2D1_RECT_F scroll_bar = {};

    D2D1_RECT_F confirm_card = {};
    D2D1_RECT_F confirm_check = {};
    D2D1_RECT_F confirm_cancel = {};
    D2D1_RECT_F confirm_ok = {};

    D2D1_RECT_F busy_card = {};
    D2D1_RECT_F busy_title = {};
    D2D1_RECT_F busy_list = {};
    D2D1_RECT_F busy_cancel = {};
    D2D1_RECT_F busy_release = {};
    float busy_scroll_max = 0.0f;

    D2D1_RECT_F admin_card = {};
    D2D1_RECT_F admin_cancel = {};
    D2D1_RECT_F admin_elevate = {};
};

class MainView {
public:
    struct Model {
        std::shared_ptr<const std::vector<Device>> devices;
        std::shared_ptr<const std::vector<Occupant>> occupants;
        std::uint32_t ejecting_disk = 0;
        std::uint32_t busy_disk = 0;  // 正在显示占用弹层的磁盘
        DialogKind dialog = DialogKind::None;
        std::wstring busy_title;
        bool busy_scanning = false;
        bool busy_releasing = false;
        std::wstring admin_detail;
        bool confirm_dont_ask = false;
        int capacity_base = 1024;
        std::vector<std::wstring> mounted_volumes;
    };

    MainView() = default;
    MainView(const MainView&) = delete;
    MainView& operator=(const MainView&) = delete;

    static constexpr float kPanelWidth = 372.0f;
    static constexpr float kPanelRadius = 16.0f;

    void Attach(HWND hwnd) { hwnd_ = hwnd; }
    void SetPalette(const Palette& palette) { palette_ = palette; }
    // 开启「贴边收起」时，拖动被限制在工作区内，碰到边缘立即停住。
    void SetEdgeClamping(bool enabled) { edge_clamping_ = enabled; }
    void SetFonts(const Fonts* fonts) { fonts_ = fonts; }
    void SetDWrite(IDWriteFactory* factory) { dwrite_ = factory; }
    void SetModel(const Model& model);

    const Model& model() const { return model_; }
    const ViewLayout& layout() const { return layout_; }

    float content_height() const;
    float preferred_panel_height(float max_panel_height) const;
    void Layout(float panel_height, float scale);

    void Paint(Canvas& canvas, float dpi, float window_alpha) const;

    // 动画推进（约 60fps）。返回 true 表示还需要下一帧。
    bool Tick();
    bool Animating() const;
    bool ToastVisible() const { return toast_opacity_ > 0.01f; }
    void ShowToast(std::wstring text, bool ok);
    void HideToast();
    void ResetHover();

    // 统一指针输入：鼠标与触摸共用同一套按下/移动/抬起判定。
    // 触摸可拖动内容与滚动条；鼠标拖动标题栏移动窗口，不改滚动位置。
    ViewAction OnPointerDown(float x, float y, PointerSource source);
    ViewAction OnPointerMove(float x, float y, PointerSource source);
    ViewAction OnPointerUp(float x, float y, PointerSource source);
    void OnMouseLeave();
    bool OnWheel(float delta);
    void CancelInteraction();

    Hit HitTest(float x, float y) const;
    bool OverInteractive(float x, float y) const;
    bool OverPanel(float x, float y) const;
    float scale() const { return scale_; }

private:
    void BuildLayout(float panel_height);
    void PaintHeader(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush) const;
    void PaintDeviceCard(ID2D1RenderTarget* target,
                         ID2D1SolidColorBrush* brush,
                         const Device& device, std::size_t index,
                         const CardLayout& card) const;
    void PaintEmptyState(ID2D1RenderTarget* target,
                         ID2D1SolidColorBrush* brush) const;
    void PaintScrollBar(ID2D1RenderTarget* target,
                        ID2D1SolidColorBrush* brush) const;
    void PaintToast(ID2D1RenderTarget* target,
                    ID2D1SolidColorBrush* brush) const;
    void PaintConfirmDialog(ID2D1RenderTarget* target,
                            ID2D1SolidColorBrush* brush) const;
    void PaintBusyDialog(ID2D1RenderTarget* target,
                         ID2D1SolidColorBrush* brush) const;
    void PaintAdminDialog(ID2D1RenderTarget* target,
                          ID2D1SolidColorBrush* brush) const;

    float ScrollThumbHeight() const;
    D2D1_RECT_F ScrollThumbRect() const;
    void SetHover(const Hit& hit);
    void SetScrollTarget(float value);
    void BeginScrollDrag(float y);
    float HoverEject(std::uint32_t disk_number) const;
    float HoverOpen(wchar_t letter) const;
    float HoverVolume(const std::wstring& volume_path) const;
    bool IsManagedMount(const std::wstring& volume_path) const;
    static bool IsMountable(const Partition& part);

    HWND hwnd_ = nullptr;
    Palette palette_ = MakePalette(false);
    const Fonts* fonts_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    Model model_;
    ViewLayout layout_;

    float scale_ = 1.0f;
    float scroll_y_ = 0.0f;
    float scroll_target_ = 0.0f;
    float busy_scroll_ = 0.0f;
    float busy_scroll_target_ = 0.0f;
    float toast_opacity_ = 0.0f;
    float toast_target_ = 0.0f;
    std::wstring toast_text_;
    bool toast_ok_ = true;

    Hit hovered_;
    Hit pressed_;
    // 统一交互状态：同一时刻只允许一个进行中的手势。
    enum class Interaction { None, Pressed, ScrollDrag, WindowDrag, Pan };
    Interaction interaction_ = Interaction::None;
    // 触摸拖动候选：按下先当作轻点，移动超过阈值才转为 Pan。
    bool pan_started_ = false;
    float press_y_ = 0.0f;
    float press_scroll_ = 0.0f;
    // 最近一次鼠标移动的位置（屏幕坐标）。设备列表定时刷新会重建布局，
    // 仅靠索引无法判断指针下还是不是同一个按钮，需要按此位置重新命中。
    POINT pointer_screen_ = {};
    bool pointer_inside_ = false;
    bool hover_revalidate_ = false;
    bool scroll_hover_ = false;
    float scroll_drag_start_y_ = 0.0f;
    float scroll_drag_start_target_ = 0.0f;
    float scroll_grow_ = 0.0f;

    POINT drag_cursor_start_ = {};
    RECT drag_window_start_ = {};
    bool edge_clamping_ = false;

    std::unordered_map<std::uint32_t, float> hover_eject_;
    std::unordered_map<wchar_t, float> hover_open_;
    std::unordered_map<std::wstring, float> hover_volume_;
    float hover_close_ = 0.0f;
    float hover_close_target_ = 0.0f;
    // 刚移入按钮时悬停动画值仍是 0，仅靠 Animating() 里的差值判断会误判
    // 为“无需动画”，导致计时器不启动。置位表示下一帧必须推进一次动画。
    bool hover_anim_pending_ = false;
};

}  // namespace usb::ui
