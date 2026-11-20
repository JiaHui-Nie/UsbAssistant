// 高 DPI 与多显示器定位的公共小工具（仅新窗口使用）。
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace usb::ui {

inline float SystemDpi() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) : 96.0f;
}

inline float WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            fn = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(user32, "GetDpiForWindow")));
        }
    }
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) return static_cast<float>(dpi);
    }
    return SystemDpi();
}

inline void EnableDpiAwareness() {
    using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetDpiContextFn>(
            reinterpret_cast<void*>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (fn && fn(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) {
            return;  // PER_MONITOR_AWARE_V2
        }
        auto legacy = reinterpret_cast<BOOL(WINAPI*)()>(
            reinterpret_cast<void*>(
                GetProcAddress(user32, "SetProcessDPIAware")));
        if (legacy) legacy();
    }
}

inline RECT WorkAreaOfMonitor(HMONITOR monitor) {
    if (monitor) {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    }
    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

// 弹窗定位参考：优先贴着参照窗口所在显示器，没有参照时用鼠标所在显示器。
inline RECT ReferenceWorkArea(HWND reference) {
    if (reference && IsWindowVisible(reference)) {
        return WorkAreaOfMonitor(
            MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST));
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    return WorkAreaOfMonitor(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
}

// 在参考显示器工作区里居中。
inline POINT CenterInWorkArea(const RECT& work_area, int width, int height) {
    POINT origin = {};
    origin.x = work_area.left + ((work_area.right - work_area.left) - width) / 2;
    origin.y = work_area.top + ((work_area.bottom - work_area.top) - height) / 2;
    origin.x = std::max(static_cast<LONG>(work_area.left), origin.x);
    origin.y = std::max(static_cast<LONG>(work_area.top), origin.y);
    return origin;
}

}  // namespace usb::ui
