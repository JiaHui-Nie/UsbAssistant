// 自绘的单按钮提示弹窗：系统 MessageBox 无法自定义按钮文案，这里统一自绘。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

#include "ui/text.h"
#include "ui/theme.h"

namespace usb::ui {

class MessageDialog {
public:
    MessageDialog() = default;
    ~MessageDialog();
    MessageDialog(const MessageDialog&) = delete;
    MessageDialog& operator=(const MessageDialog&) = delete;

    bool Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
              IDWriteFactory* dwrite, const Fonts* fonts,
              const Palette& palette, std::wstring title,
              std::wstring message, std::wstring button_text, bool top_most);
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }

private:
    enum class Hover { None, Close, Button };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    void ApplyScale();
    void ResizeWindow();
    bool EnsureTarget();
    void ReleaseTarget();
    void Paint();

    float Width() const;
    float Height() const;
    D2D1_RECT_F CloseRect() const;
    D2D1_RECT_F ButtonRect() const;
    D2D1_RECT_F MessageRect() const;
    Hover HitControl(float x, float y) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    const Fonts* fonts_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    Palette palette_ = MakePalette(false);
    std::wstring title_;
    std::wstring message_;
    std::wstring button_text_;
    float message_height_ = 18.0f;

    float scale_ = 1.0f;
    Hover hover_ = Hover::None;
    bool tracking_leave_ = false;
    bool dragging_ = false;
    POINT drag_cursor_start_ = {};
    RECT drag_window_start_ = {};
};

}  // namespace usb::ui
