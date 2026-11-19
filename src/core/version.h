// 构建时注入的版本号与项目地址。
// 版本号由 CMake 从 git tag 自动推导（见 CMakeLists.txt），
// 源码里不重复维护版本字符串。
#pragma once

#ifndef USBASSISTANT_VERSION
#define USBASSISTANT_VERSION L"0.0.0"
#endif

#ifndef USBASSISTANT_REPO_URL
#define USBASSISTANT_REPO_URL L"https://github.com/JiaHui-Nie/UsbAssistant"
#endif

namespace usb {

// 用于比较/请求头的纯数字版本，如 "1.2.3"。
inline constexpr const wchar_t* kAppVersion = USBASSISTANT_VERSION;

#if defined(USBASSISTANT_DEBUG_BUILD)
// Debug 构建不伪装成正式版本号，只说明是开发版。
// 关于界面程序名下方显示的版本：Debug 只显示 dev。
inline constexpr const wchar_t* kAboutVersion = L"dev";
inline constexpr bool kDebugBuild = true;
#else
// 关于界面程序名下方显示的版本：Release 只显示纯数字版本号，例如 1.0.0。
inline constexpr const wchar_t* kAboutVersion = USBASSISTANT_VERSION;
inline constexpr bool kDebugBuild = false;
#endif

#if defined(USBASSISTANT_DEV_BUILD)
inline constexpr bool kDevBuild = true;
#else
inline constexpr bool kDevBuild = false;
#endif

inline constexpr const wchar_t* kRepositoryUrl = USBASSISTANT_REPO_URL;
inline constexpr const wchar_t* kLatestReleaseApiUrl =
    L"https://api.github.com/repos/JiaHui-Nie/UsbAssistant/releases/latest";

}  // namespace usb
