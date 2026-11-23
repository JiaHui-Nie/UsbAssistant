// 构建时注入的版本号与项目地址。
// 版本号来源由 CMake 决定（-DUSBASSISTANT_VERSION_OVERRIDE 或 tag 正好指向
// HEAD），源码里不重复维护版本字符串。
#pragma once

#ifndef USBASSISTANT_VERSION
#define USBASSISTANT_VERSION L"0.0.0"
#endif

// 对外展示的版本标识：有版本来源时是数字版本号，否则是 dev。
// 未由 CMake 注入时退回数字版本，避免出现无法比较的值。
#ifndef USBASSISTANT_DISPLAY_VERSION
#define USBASSISTANT_DISPLAY_VERSION USBASSISTANT_VERSION
#endif

#ifndef USBASSISTANT_REPO_URL
#define USBASSISTANT_REPO_URL L"https://github.com/JiaHui-Nie/UsbAssistant"
#endif

namespace usb {

// 用于版本比较与更新判定的数字版本号，如 "1.2.3"。
// 即使对外展示 dev，这里也始终是数字。
inline constexpr const wchar_t* kAppVersion = USBASSISTANT_VERSION;

// 关于界面与 User-Agent 等对外展示的版本标识：数字版本号或 dev。
// 只由是否有版本来源决定，与 Debug / Release 构建类型无关。
inline constexpr const wchar_t* kAboutVersion = USBASSISTANT_DISPLAY_VERSION;

// 发布包维度：配置（debug / release）与架构（x64 / x86）。
// 用于在 GitHub Release 资产里匹配与自身完全对应的 zip。
#if defined(USBASSISTANT_DEBUG_BUILD)
inline constexpr const wchar_t* kBuildConfigLabel = L"debug";
#else
inline constexpr const wchar_t* kBuildConfigLabel = L"release";
#endif

#if defined(_M_X64) || defined(__x86_64__)
inline constexpr const wchar_t* kBuildArchLabel = L"x64";
#else
inline constexpr const wchar_t* kBuildArchLabel = L"x86";
#endif

inline constexpr const wchar_t* kRepositoryUrl = USBASSISTANT_REPO_URL;
inline constexpr const wchar_t* kLatestReleaseApiUrl =
    L"https://api.github.com/repos/JiaHui-Nie/UsbAssistant/releases/latest";

}  // namespace usb
