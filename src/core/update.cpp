#include "core/update.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/version.h"

namespace usb {
namespace {

constexpr std::size_t kMaxResponseBytes = 1024u * 1024u;

struct WinHttpHandle {
    HINTERNET value = nullptr;
    WinHttpHandle() = default;
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
};

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) return L"";
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

// 从 JSON 中取一个字符串字段，只处理更新接口用到的简单形式。
bool FindJsonString(const std::string& json, const char* key,
                    std::string* out) {
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' ||
            json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;

    std::string value;
    while (pos < json.size()) {
        const char ch = json[pos++];
        if (ch == '"') {
            *out = std::move(value);
            return true;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (pos >= json.size()) return false;
        const char escaped = json[pos++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
                // 版本号与 URL 不含非 ASCII 字符，跳过 \uXXXX 即可。
                if (pos + 4 > json.size()) return false;
                pos += 4;
                value.push_back('?');
                break;
            default:
                return false;
        }
    }
    return false;
}

std::vector<unsigned long long> ParseNumericComponents(
    const std::wstring& text) {
    std::vector<unsigned long long> parts;
    std::size_t index = 0;
    while (index < text.size()) {
        const wchar_t ch = text[index];
        if (ch >= L'0' && ch <= L'9') {
            unsigned long long value = 0;
            while (index < text.size() && text[index] >= L'0' &&
                   text[index] <= L'9') {
                value = value * 10ull +
                        static_cast<unsigned long long>(text[index] - L'0');
                ++index;
            }
            parts.push_back(value);
            continue;
        }
        if (ch == L'.') {
            ++index;
            continue;
        }
        // 形如 1.2.3-beta：预发布后缀不参与比较。
        break;
    }
    return parts;
}

bool IsWebUrl(const std::wstring& url) {
    return url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0;
}

UpdateCheckResult NetworkFailure(const std::wstring& current,
                                 const std::wstring& message) {
    UpdateCheckResult result;
    result.status = UpdateCheckResult::Status::NetworkError;
    result.current_version = current;
    result.message = message;
    return result;
}

}  // namespace

std::wstring NormalizeVersion(const std::wstring& tag) {
    std::size_t begin = 0;
    std::size_t end = tag.size();
    while (begin < end && (tag[begin] == L' ' || tag[begin] == L'\t' ||
                           tag[begin] == L'\r' || tag[begin] == L'\n')) {
        ++begin;
    }
    while (end > begin && (tag[end - 1] == L' ' || tag[end - 1] == L'\t' ||
                           tag[end - 1] == L'\r' || tag[end - 1] == L'\n')) {
        --end;
    }
    std::wstring text = tag.substr(begin, end - begin);
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) {
        text.erase(text.begin());
    }
    return text;
}

int CompareVersions(const std::wstring& left, const std::wstring& right) {
    const std::vector<unsigned long long> lhs =
        ParseNumericComponents(NormalizeVersion(left));
    const std::vector<unsigned long long> rhs =
        ParseNumericComponents(NormalizeVersion(right));
    if (lhs.empty() || rhs.empty()) return 0;

    const std::size_t count = std::max(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned long long a = i < lhs.size() ? lhs[i] : 0ull;
        const unsigned long long b = i < rhs.size() ? rhs[i] : 0ull;
        if (a < b) return -1;
        if (a > b) return 1;
    }
    return 0;
}

bool ParseLatestRelease(const std::string& json, std::wstring* tag,
                        std::wstring* html_url, std::wstring* error) {
    std::string tag_utf8;
    std::string url_utf8;
    if (!FindJsonString(json, "tag_name", &tag_utf8) || tag_utf8.empty()) {
        if (error) *error = L"缺少 tag_name";
        return false;
    }
    if (!FindJsonString(json, "html_url", &url_utf8) || url_utf8.empty()) {
        if (error) *error = L"缺少 html_url";
        return false;
    }

    std::wstring tag_wide = Utf8ToWide(tag_utf8);
    std::wstring url_wide = Utf8ToWide(url_utf8);
    if (tag_wide.empty() ||
        ParseNumericComponents(NormalizeVersion(tag_wide)).empty()) {
        if (error) *error = L"版本号格式无效";
        return false;
    }
    if (!IsWebUrl(url_wide)) {
        if (error) *error = L"Release 链接格式无效";
        return false;
    }

    if (tag) *tag = std::move(tag_wide);
    if (html_url) *html_url = std::move(url_wide);
    return true;
}

UpdateCheckResult CheckLatestRelease(const std::wstring& current_version,
                                     int timeout_ms) {
    const std::wstring current = NormalizeVersion(current_version);
    const int timeout = std::clamp(timeout_ms, 1000, 60000);

    const std::wstring user_agent =
        L"UsbAssistant/" +
        std::wstring(kAppVersion[0] ? kAppVersion : L"unknown");

    WinHttpHandle session;
    session.value =
        WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        return NetworkFailure(current, L"检查失败：无法初始化网络请求");
    }
    WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout);

    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(kLatestReleaseApiUrl, 0, 0, &components) ||
        !components.lpszHostName) {
        return NetworkFailure(current, L"检查失败：更新地址无效");
    }
    const std::wstring host(components.lpszHostName,
                            components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    WinHttpHandle connection;
    connection.value = WinHttpConnect(session.value, host.c_str(),
                                      components.nPort, 0);
    if (!connection.value) {
        return NetworkFailure(current, L"检查失败：无法连接更新服务器");
    }

    WinHttpHandle request;
    request.value = WinHttpOpenRequest(
        connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.value) {
        return NetworkFailure(current, L"检查失败：无法创建更新请求");
    }

    const std::wstring headers =
        L"Accept: application/vnd.github+json\r\n"
        L"User-Agent: " +
        user_agent + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpAddRequestHeaders(
            request.value, headers.c_str(), static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return NetworkFailure(current, L"检查失败：无法设置请求头");
    }

    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        return NetworkFailure(current,
                              L"检查失败：网络不可用或请求超时，请稍后重试");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return NetworkFailure(current, L"检查失败：无法读取服务器响应");
    }
    if (status != 200) {
        return NetworkFailure(current, L"检查失败：更新服务器返回 HTTP " +
                                           std::to_wstring(status));
    }

    std::string body;
    bool read_ok = true;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            read_ok = false;
            break;
        }
        if (available == 0) break;
        const std::size_t offset = body.size();
        const std::size_t room = kMaxResponseBytes - offset;
        if (room == 0) break;
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(available, room));
        body.resize(offset + want);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, body.data() + offset, want,
                             &read)) {
            read_ok = false;
            break;
        }
        body.resize(offset + read);
        if (read == 0) break;
    }
    if (!read_ok) {
        return NetworkFailure(current, L"检查失败：读取更新信息中断");
    }

    std::wstring tag;
    std::wstring url;
    std::wstring parse_error;
    if (!ParseLatestRelease(body, &tag, &url, &parse_error)) {
        UpdateCheckResult result;
        result.status = UpdateCheckResult::Status::ParseError;
        result.current_version = current;
        result.message = L"检查失败：无法解析更新信息";
        return result;
    }

    const std::wstring latest = NormalizeVersion(tag);
    UpdateCheckResult result;
    result.current_version = current;
    result.latest_version = latest;
    result.release_url = std::move(url);
    if (ParseNumericComponents(latest).empty()) {
        result.status = UpdateCheckResult::Status::ParseError;
        result.message = L"检查失败：无法解析更新信息";
        return result;
    }
    if (CompareVersions(current, latest) >= 0) {
        result.status = UpdateCheckResult::Status::UpToDate;
        result.message = L"已是最新版本（v" + latest + L"）";
    } else {
        result.status = UpdateCheckResult::Status::UpdateAvailable;
        result.message = L"发现新版本 v" + latest;
    }
    return result;
}

}  // namespace usb
