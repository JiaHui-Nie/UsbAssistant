#include "core/http.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/version.h"

namespace usb::http {
namespace {

constexpr std::size_t kMaxTextBytes = 1024u * 1024u;
constexpr DWORD kReadChunk = 64u * 1024u;

struct WinHttpHandle {
    HINTERNET value = nullptr;
    WinHttpHandle() = default;
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
};

struct Endpoint {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

std::wstring UserAgent() {
    return L"UsbAssistant/" + std::wstring(kAboutVersion);
}

bool CrackUrl(const std::wstring& url, Endpoint* endpoint) {
    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
                         &components) ||
        !components.lpszHostName || components.dwHostNameLength == 0) {
        return false;
    }
    endpoint->host.assign(components.lpszHostName,
                          components.dwHostNameLength);
    endpoint->path.assign(components.lpszUrlPath,
                          components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo) {
        endpoint->path.append(components.lpszExtraInfo,
                              components.dwExtraInfoLength);
    }
    endpoint->port = components.nPort;
    endpoint->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

// 打开请求并完成发送与响应头接收，状态码必须为 200。
bool Perform(const std::wstring& url, const std::wstring& accept,
             int timeout_ms, WinHttpHandle* session,
             WinHttpHandle* connection, WinHttpHandle* request,
             std::wstring* error) {
    Endpoint endpoint;
    if (!CrackUrl(url, &endpoint)) {
        if (error) *error = L"更新地址无效";
        return false;
    }

    const std::wstring agent = UserAgent();
    session->value = WinHttpOpen(agent.c_str(),
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session->value) {
        if (error) *error = L"无法初始化网络请求";
        return false;
    }
    WinHttpSetTimeouts(session->value, timeout_ms, timeout_ms, timeout_ms,
                       timeout_ms);

    connection->value = WinHttpConnect(session->value, endpoint.host.c_str(),
                                       endpoint.port, 0);
    if (!connection->value) {
        if (error) *error = L"无法连接服务器";
        return false;
    }

    request->value = WinHttpOpenRequest(
        connection->value, L"GET", endpoint.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request->value) {
        if (error) *error = L"无法创建网络请求";
        return false;
    }

    // GitHub Release 资产会 302 到对象存储，必须显式允许跟随重定向。
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request->value, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    const std::wstring headers = L"Accept: " + accept +
                                 L"\r\nUser-Agent: " + agent +
                                 L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpAddRequestHeaders(
            request->value, headers.c_str(), static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        if (error) *error = L"无法设置请求头";
        return false;
    }

    if (!WinHttpSendRequest(request->value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request->value, nullptr)) {
        if (error) *error = L"网络不可用或请求超时";
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request->value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        if (error) *error = L"无法读取服务器响应";
        return false;
    }
    if (status != 200) {
        if (error) {
            *error = L"服务器返回 HTTP " + std::to_wstring(status);
        }
        return false;
    }
    return true;
}

}  // namespace

bool GetText(const std::wstring& url, int timeout_ms, std::string* body,
             std::wstring* error) {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle request;
    if (!Perform(url, L"application/vnd.github+json", timeout_ms, &session,
                 &connection, &request, error)) {
        return false;
    }

    std::string text;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            if (error) *error = L"读取响应中断";
            return false;
        }
        if (available == 0) break;

        const std::size_t room = kMaxTextBytes - text.size();
        if (room == 0) break;
        const DWORD want =
            static_cast<DWORD>(std::min<std::size_t>(available, room));
        const std::size_t offset = text.size();
        text.resize(offset + want);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, text.data() + offset, want,
                             &read)) {
            if (error) *error = L"读取响应中断";
            return false;
        }
        text.resize(offset + read);
        if (read == 0) break;
    }
    if (body) *body = std::move(text);
    return true;
}

bool Download(const std::wstring& url, const std::wstring& path,
              int timeout_ms, const ProgressHandler& progress,
              const std::atomic_bool* cancel, std::wstring* error) {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle request;
    if (!Perform(url, L"application/octet-stream", timeout_ms, &session,
                 &connection, &request, error)) {
        return false;
    }

    Progress state;
    DWORD content_length = 0;
    DWORD length_size = sizeof(content_length);
    if (WinHttpQueryHeaders(request.value,
                            WINHTTP_QUERY_CONTENT_LENGTH |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &content_length,
                            &length_size, WINHTTP_NO_HEADER_INDEX) &&
        content_length > 0) {
        state.total = content_length;
        state.total_known = true;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"无法写入临时文件";
        return false;
    }
    if (progress) progress(state);

    std::vector<std::uint8_t> buffer(kReadChunk);
    std::uint64_t last_report = GetTickCount64();
    bool ok = true;
    for (;;) {
        if (cancel && cancel->load()) {
            if (error) *error = L"已取消";
            ok = false;
            break;
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            if (error) *error = L"下载中断";
            ok = false;
            break;
        }
        if (available == 0) break;

        const DWORD want =
            static_cast<DWORD>(std::min<std::size_t>(available, kReadChunk));
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), want, &read)) {
            if (error) *error = L"下载中断";
            ok = false;
            break;
        }
        if (read == 0) break;

        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
            written != read) {
            if (error) *error = L"写入临时文件失败";
            ok = false;
            break;
        }
        state.received += read;

        const std::uint64_t now = GetTickCount64();
        if (progress && (now - last_report >= 100 ||
                         (state.total_known && state.received >= state.total))) {
            last_report = now;
            progress(state);
        }
    }
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(path.c_str());
        return false;
    }
    if (state.total_known && state.received != state.total) {
        USB_LOG_WARN(L"http", L"下载长度与 Content-Length 不一致：%llu/%llu",
                     static_cast<unsigned long long>(state.received),
                     static_cast<unsigned long long>(state.total));
    }
    if (progress) progress(state);
    return true;
}

}  // namespace usb::http
