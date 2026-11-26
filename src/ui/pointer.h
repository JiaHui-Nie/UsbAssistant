// 指针输入：鼠标与触摸共用同一套“按下 / 移动 / 抬起”判定。
#pragma once

#include <windows.h>

namespace usb::ui {

enum class PointerSource { Mouse, Touch };

// Windows 会把触摸/笔合成的鼠标消息标记在 GetMessageExtraInfo() 里：
// 低位为 MOUSEEVENTF_FROMTOUCH(0xFF515700)，触摸与笔的高位相同。
// 用它精确忽略合成鼠标，避免用时间窗误伤真实鼠标。
inline bool IsPromotedPointerMouse() {
    const ULONG_PTR extra = GetMessageExtraInfo();
    return (extra & static_cast<ULONG_PTR>(0xFFFFFF00u)) ==
           static_cast<ULONG_PTR>(0xFF515700u);
}

}  // namespace usb::ui
