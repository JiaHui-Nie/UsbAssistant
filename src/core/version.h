// 构建时注入的版本号与项目地址。
// 默认构建为 dev 版；只有显式指定正式版本号（见 CMakeLists.txt）才会注入
// 数字版本，源码里不重复维护版本字符串。
#pragma once

#ifndef USBASSISTANT_VERSION
#define USBASSISTANT_VERSION L"dev"
#endif

#ifndef USBASSISTANT_REPO_URL
#define USBASSISTANT_REPO_URL L"https://github.com/JiaHui-Nie/UsbAssistant"
#endif

namespace usb {

// 用于版本比较的版本标识：正式版是纯数字（如 "1.2.3"），dev 版为 "dev"。
// dev 版没有数字顺序，更新判定由 updater 单独处理。
inline constexpr const wchar_t* kAppVersion = USBASSISTANT_VERSION;

#if defined(USBASSISTANT_DEBUG_BUILD) || defined(USBASSISTANT_DEV_BUILD)
// 开发版（默认构建、Debug、未指定正式版本号）对外只显示 dev。
inline constexpr const wchar_t* kAboutVersion = L"dev";
#else
// 正式版：只显示纯数字版本号，例如 1.2.1。
inline constexpr const wchar_t* kAboutVersion = USBASSISTANT_VERSION;
#endif

#if defined(USBASSISTANT_DEV_BUILD)
inline constexpr bool kDevBuild = true;
#else
inline constexpr bool kDevBuild = false;
#endif

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
