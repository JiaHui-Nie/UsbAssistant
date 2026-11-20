// 版本比较、GitHub Releases 查询与发布包匹配。
// 网络请求是同步的，调用方必须放到后台线程，避免阻塞界面。
#pragma once

#include <string>
#include <vector>

namespace usb {

struct ReleaseAsset {
    std::wstring name;
    std::wstring download_url;
};

struct LatestRelease {
    std::wstring tag;      // 原始 tag，如 v1.2.1
    std::wstring version;  // 去掉前导 v 的纯数字版本
    std::wstring html_url;
    std::vector<ReleaseAsset> assets;
};

enum class ReleaseFetchStatus {
    Ok,
    NetworkError,
    ParseError,
};

// 去掉首尾空白与前导 v/V，得到 1.2.3 这样的形式。
std::wstring NormalizeVersion(const std::wstring& tag);

// 逐段按数字比较，缺段视为 0；任一侧没有可解析的数字段时返回 0。
int CompareVersions(const std::wstring& left, const std::wstring& right);

// 从 GitHub Releases API 的 JSON 中取 tag_name 与 html_url。
// 失败时返回 false，并在 error 中给出原因。
bool ParseLatestRelease(const std::string& json, std::wstring* tag,
                        std::wstring* html_url, std::wstring* error);

// 解析 assets 数组中的文件名与下载地址；没有可用资产时返回 false。
bool ParseReleaseAssets(const std::string& json,
                        std::vector<ReleaseAsset>* assets);

// 请求 GitHub 最新 release：tag、版本与可选资产列表。
ReleaseFetchStatus FetchLatestRelease(LatestRelease* release,
                                      std::wstring* error,
                                      int timeout_ms = 8000);

// 发布包文件名后缀，如 "-debug-x64.zip"。配置与架构在编译期确定。
std::wstring ReleaseAssetSuffix(const std::wstring& config_label,
                                const std::wstring& arch_label);

// 找出与自身配置/架构完全匹配的资产（后缀匹配，不会拿错维度）。
bool SelectReleaseAsset(const LatestRelease& release,
                        const std::wstring& config_label,
                        const std::wstring& arch_label, ReleaseAsset* asset);

}  // namespace usb
