// 关于窗口：自绘卡片，展示版本、项目地址，并支持检查更新。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>

#include "core/update.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace usb::ui {

class AboutWindow {
public:
    using CheckHandler = std::function<void()>;
    using CloseHandler = std::function<void()>;

    AboutWindow() = default;
    ~AboutWindow();
    AboutWindow(const AboutWindow&) = delete;
    AboutWindow& operator=(const AboutWindow&) = delete;

    bool Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
              IDWriteFactory* dwrite, const Fonts* fonts,
              const Palette& palette, std::wstring version,
              CheckHandler on_check, CloseHandler on_close = {});
    void Close();
    bool IsOpen() const;

    // 与主窗口的「始终置顶」保持一致。
    void SetTopMost(bool enabled);
    void SetPalette(const Palette& palette);

    void SetChecking(bool checking);
    void SetUpdateResult(const UpdateCheckResult& result);

private:
    enum class Hover {
        None,
        Close,
        Check,
        Repository,
        Release,
    };

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
                     const std::wstring& text, bool primary, bool hovered,
                     bool enabled);
    void OpenUrl(const std::wstring& url);

    D2D1_RECT_F CloseRect() const;
    D2D1_RECT_F CheckRect() const;
    D2D1_RECT_F RepositoryLinkRect() const;
    D2D1_RECT_F StatusRect() const;
    Hover HitControl(float x, float y) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    const Fonts* fonts_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    Palette palette_ = MakePalette(false);
    std::wstring version_;
    CheckHandler on_check_;
    CloseHandler on_close_;

    UpdateCheckResult result_ = {};
    bool has_result_ = false;
    bool checking_ = false;

    float scale_ = 1.0f;
    Hover hover_ = Hover::None;
    bool tracking_leave_ = false;
    bool dragging_ = false;
    POINT drag_cursor_start_ = {};
    RECT drag_window_start_ = {};
};

}  // namespace usb::ui
