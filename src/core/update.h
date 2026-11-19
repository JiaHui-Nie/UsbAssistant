// 版本比较与 GitHub Releases 更新检查。
// 网络请求是同步的，调用方必须放到后台线程，避免阻塞界面。
#pragma once

#include <string>

namespace usb {

struct UpdateCheckResult {
    enum class Status {
        UpToDate,
        UpdateAvailable,
        NetworkError,
        ParseError,
    };

    Status status = Status::NetworkError;
    std::wstring current_version;
    std::wstring latest_version;
    std::wstring release_url;
    std::wstring message;
};

// 去掉首尾空白与前导 v/V，得到 1.2.3 这样的形式。
std::wstring NormalizeVersion(const std::wstring& tag);

// 逐段按数字比较，缺段视为 0；任一侧没有可解析的数字段时返回 0。
int CompareVersions(const std::wstring& left, const std::wstring& right);

// 从 GitHub Releases API 的 JSON 中取 tag_name 与 html_url。
// 失败时返回 false，并在 error 中给出原因。
bool ParseLatestRelease(const std::string& json, std::wstring* tag,
                        std::wstring* html_url, std::wstring* error);

// 请求 GitHub 最新 release 并与 current_version 比较。
// timeout_ms 同时用于连接、发送与接收超时。
UpdateCheckResult CheckLatestRelease(const std::wstring& current_version,
                                     int timeout_ms = 8000);

}  // namespace usb
