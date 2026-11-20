// 基于 WinHTTP 的最小 HTTP 客户端：文本响应与带进度的文件下载。
// 所有函数都是同步的，调用方必须放到后台线程。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace usb::http {

struct Progress {
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    bool total_known = false;
};

using ProgressHandler = std::function<void(const Progress&)>;

// 只接受 http/https 地址；成功时返回 HTTP 200 的响应体。
bool GetText(const std::wstring& url, int timeout_ms, std::string* body,
             std::wstring* error);

// 下载到文件（覆盖写入）。cancel 置位时尽快中断并删除半成品。
// 服务器没给 Content-Length 时 total_known 为 false，进度退化为只有已收字节。
bool Download(const std::wstring& url, const std::wstring& path,
              int timeout_ms, const ProgressHandler& progress,
              const std::atomic_bool* cancel, std::wstring* error);

}  // namespace usb::http
