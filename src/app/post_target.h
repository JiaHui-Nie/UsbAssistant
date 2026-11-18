// 后台任务回传消息的目标窗口。WM_DESTROY 会清空 hwnd，
// 从而保证迟到的后台结果不会 Post 到已销毁（甚至被复用）的窗口句柄。
#pragma once

#include <windows.h>

#include <mutex>

namespace usb::app {

struct PostTarget {
    std::mutex mutex;
    HWND hwnd = nullptr;
};

}  // namespace usb::app
