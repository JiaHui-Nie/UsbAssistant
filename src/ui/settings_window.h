// 设置窗口：自绘行式布局，选项用原生弹出菜单，不引入任何控件库。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>
#include <vector>

#include "core/settings.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace usb::ui {

class SettingsWindow {
public:
    using ChangeHandler = std::function<void(const Settings&)>;
    // 校验并注册全局快捷键；成功返回空字符串，失败返回给用户看的原因。
    using HotkeyHandler = std::function<std::wstring(int mods, int vk)>;
    using CloseHandler = std::function<void()>;

    SettingsWindow() = default;
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    bool Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
              IDWriteFactory* dwrite, const Fonts* fonts,
              const Palette& palette, const Settings& settings,
              ChangeHandler on_change, HotkeyHandler on_hotkey = {},
              CloseHandler on_close = {});
    void Close();
    bool IsOpen() const;

    // 与主窗口的「始终置顶」保持一致：开启时设置窗口也置顶。
    // 打开时临时置顶并激活，随后恢复普通层级；不做永久置顶。
    void RaiseToFront();

    void SetPalette(const Palette& palette);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    void ApplyScale();
    void ResizeWindow();
    bool EnsureTarget();
    void ReleaseTarget();
    void Paint();
    void PaintRows(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush);
    void PaintRow(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                  int row, const D2D1_RECT_F& rect, bool hovered,
                  bool enabled);

    D2D1_RECT_F RowRect(int row) const;
    std::vector<int> RowOrder() const;
    float ContentHeight() const;
    float BaseContentHeight() const;
    void UpdateScrollBounds();
    static bool IsSubRow(int row);
    static int SubRowParent(int row);
    static bool SubRowIsChoice(int row);
    static const wchar_t* SubRowLabel(int row);
    // 带展开箭头的父行：显示无盘符分区、自动更新。
    static bool IsExpandable(int row);
    bool IsExpanded(int row) const;
    void ToggleExpanded(int row);
    void PaintChoice(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
                     int row, bool enabled);
    D2D1_RECT_F ToggleRect(int row) const;
    D2D1_RECT_F ChoiceRect(int row) const;
    D2D1_RECT_F ShortcutRect(int row) const;
    D2D1_RECT_F ShortcutClearRect(int row) const;
    D2D1_RECT_F CloseRect() const;
    int HitRow(float x, float y) const;

    void ToggleRow(int row);
    void ChooseRow(int row);
    void CancelHotkeyCaptureAt(float x, float y);
    void HandleRowClick(float x, float y);
    void HandleHotkeyCapture(int vk);
    void ClearHotkey();
    void ApplyHotkey(int mods, int vk);
    void NotifyChange();
    void SetScroll(float value);

    // 滚动条：几何与拖动（鼠标与触摸共用）。
    float ScrollBarCenterX() const;
    float ScrollTrackTop() const;
    float ScrollTrackBottom() const;
    D2D1_RECT_F ScrollTrackRect() const;
    D2D1_RECT_F ScrollThumbRect() const;
    D2D1_RECT_F ScrollBarHitRect() const;
    float ScrollThumbHeight() const;
    void BeginScrollDrag(float y);
    void UpdateScrollDrag(float y);

    // 触摸屏：内容区上下拖动滚动，滚动条区域直接拖动滑块。
    void HandleTouch(UINT count, HTOUCHINPUT handle);
    bool SyntheticMouseSuppressed() const {
        return touch_active_ ||
               static_cast<DWORD>(GetTickCount() - touch_last_tick_) < 300;
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    const Fonts* fonts_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    Palette palette_ = MakePalette(false);
    Settings settings_;
    ChangeHandler on_change_;
    HotkeyHandler on_hotkey_;
    CloseHandler on_close_;

    float scale_ = 1.0f;
    float view_height_ = 0.0f;
    float scroll_max_ = 0.0f;
    float scroll_ = 0.0f;
    int hover_row_ = -1;
    bool close_hover_ = false;
    bool clear_hover_ = false;
    bool capturing_hotkey_ = false;
    bool menu_open_ = false;
    // 两个可折叠分组的展开状态，分别对应“显示无盘符分区”和“自动检查更新”。
    bool hidden_expanded_ = false;
    bool update_expanded_ = false;
    int open_row_ = -1;
    bool tracking_leave_ = false;
    bool dragging_ = false;
    POINT drag_cursor_start_ = {};
    RECT drag_window_start_ = {};
    bool scroll_hover_ = false;
    bool scroll_dragging_ = false;
    float scroll_drag_start_y_ = 0.0f;
    float scroll_drag_start_scroll_ = 0.0f;
    // 触摸屏：只跟踪第一个触点，其余触点在抬手前忽略。
    bool touch_active_ = false;
    DWORD touch_contact_id_ = 0;
    DWORD touch_last_tick_ = 0;
    bool touch_scroll_drag_ = false;
    bool touch_pending_ = false;
    bool touch_panning_ = false;
    float touch_start_y_ = 0.0f;
    float touch_start_scroll_ = 0.0f;
};

}  // namespace usb::ui
