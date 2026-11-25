// 版本号与项目地址。
// 版本号只由命令行注入（-DUSBASSISTANT_VERSION_OVERRIDE=<x.y.z>）；没有传入
// 时是 dev。源码里不保存任何版本号。
#pragma once

#include <string>

#ifndef USBASSISTANT_VERSION
#define USBASSISTANT_VERSION L"dev"
#endif

// 对外展示的版本标识：正式构建是数字版本号，否则是 dev。
#ifndef USBASSISTANT_DISPLAY_VERSION
#define USBASSISTANT_DISPLAY_VERSION USBASSISTANT_VERSION
#endif

#ifndef USBASSISTANT_REPO_URL
#define USBASSISTANT_REPO_URL L"https://github.com/JiaHui-Nie/UsbAssistant"
#endif

namespace usb {

// 用于更新判定的版本号：正式构建是 "x.y.z" 这样的数字，开发构建是 "dev"。
// dev 没有数字段，版本比较按“相等”处理，所以开发构建不会提示更新。
inline constexpr const wchar_t* kAppVersion = USBASSISTANT_VERSION;

// 开发构建（没有数字版本号）不参与更新检查。
inline bool IsDevBuild() { return std::wstring(kAppVersion) == L"dev"; }

// 关于界面与 User-Agent 等对外展示的版本标识：数字版本号或 dev。
// 与 Debug / Release 构建类型无关。
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
