// 升级器窗口：自绘弹窗，按阶段切换提示文字、按钮与下载进度。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>

#include "ui/text.h"
#include "ui/theme.h"

namespace usb::ui {

enum class UpdaterStage {
    Checking,     // 正在检查更新中……
    CheckFailed,  // 检查更新失败。
    UpToDate,     // 已是最新版本(vX.Y.Z)。
    Available,    // 发现新版本(vX.Y.Z)。
    Downloading,  // 正在下载更新……（进度条）
    Updating,     // 正在更新……
    Failed,       // 更新失败
};

class UpdaterWindow {
public:
    using ActionHandler = std::function<void()>;
    using CloseHandler = std::function<void()>;

    UpdaterWindow() = default;
    ~UpdaterWindow();
    UpdaterWindow(const UpdaterWindow&) = delete;
    UpdaterWindow& operator=(const UpdaterWindow&) = delete;

    bool Open(HINSTANCE instance, HWND reference, bool visible,
              ID2D1Factory* d2d, IDWriteFactory* dwrite, const Fonts* fonts,
              const Palette& palette, bool top_most, ActionHandler on_update,
              CloseHandler on_close);
    void Close();
    void Show();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

    void SetStage(UpdaterStage stage, std::wstring text);
    // fraction < 0 表示长度未知，进度条改为往复动画。
    void SetProgress(float fraction);
    void CloseLater(int delay_ms);

private:
    enum class Hover { None, Close, Primary, Secondary };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    void ApplyScale();
    void ResizeWindow();
    bool EnsureTarget();
    void ReleaseTarget();
    void Paint();
    void PaintButton(ID2D1RenderTarget* target,
                     ID2D1SolidColorBrush* brush, const D2D1_RECT_F& rect,
                     const std::wstring& text, bool primary, bool hovered);
    void PaintProgress(ID2D1RenderTarget* target,
                       ID2D1SolidColorBrush* brush);
    void UpdateAnimationTimer();

    D2D1_RECT_F CloseRect() const;
    D2D1_RECT_F PrimaryRect() const;
    D2D1_RECT_F SecondaryRect() const;
    D2D1_RECT_F BodyRect() const;
    bool HasPrimary() const;
    bool HasSecondary() const;
    bool ShowsProgress() const;
    std::wstring PrimaryText() const;
    std::wstring SecondaryText() const;
    Hover HitControl(float x, float y) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    const Fonts* fonts_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    Palette palette_ = MakePalette(false);
    ActionHandler on_update_;
    CloseHandler on_close_;

    UpdaterStage stage_ = UpdaterStage::Checking;
    std::wstring text_;
    float progress_ = -1.0f;
    ULONGLONG animation_start_ = 0;
    bool animating_ = false;

    float scale_ = 1.0f;
    bool top_most_ = false;
    Hover hover_ = Hover::None;
    bool tracking_leave_ = false;
    bool dragging_ = false;
    POINT drag_cursor_start_ = {};
    RECT drag_window_start_ = {};
};

}  // namespace usb::ui
