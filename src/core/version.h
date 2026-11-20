// 构建时注入的版本号与项目地址。
// 版本号由 CMake 从 git tag 自动推导（见 CMakeLists.txt），
// 源码里不重复维护版本字符串。
#pragma once

#ifndef USBASSISTANT_VERSION
#define USBASSISTANT_VERSION L"1.2.0"
#endif

#ifndef USBASSISTANT_REPO_URL
#define USBASSISTANT_REPO_URL L"https://github.com/JiaHui-Nie/UsbAssistant"
#endif

namespace usb {

// 用于比较与请求头的纯数字版本，如 "1.2.3"；开发版也不改变它。
inline constexpr const wchar_t* kAppVersion = USBASSISTANT_VERSION;

#if defined(USBASSISTANT_DEBUG_BUILD) || defined(USBASSISTANT_DEV_BUILD)
// 开发版（Debug、分支、脏工作区、无 tag）对外只显示 dev，不带数字或 hash。
inline constexpr const wchar_t* kAboutVersion = L"dev";
#else
// 正式版（tag 正好在 HEAD）：只显示纯数字版本号，例如 1.2.0。
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
