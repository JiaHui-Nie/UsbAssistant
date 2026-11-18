// 自绘托盘右键菜单。使用 NOACTIVATE 弹出窗口 + 轮询关闭，
// 避免激活时打断 Windows 托盘的折叠飞出区。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ui/theme.h"

namespace usb::ui {

class TrayMenu {
public:
    // 返回选中项下标；-1 表示取消。items 固定为三项。
    int Show(HINSTANCE instance, HWND owner, POINT screen_point,
             const Palette& palette, float scale);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);
    void Paint(HDC dc);
    float ItemTop(int index) const;
    int HitItem(int y_px) const;
    float MenuHeight() const;
    void Finish(int choice);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Palette palette_ = MakePalette(false);
    float scale_ = 1.0f;
    int hover_ = -1;
    int choice_ = -1;
    bool done_ = false;
    std::vector<std::wstring> items_;
};

}  // namespace usb::ui
