#include "core/update.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "core/http.h"
#include "core/log.h"
#include "core/version.h"

namespace usb {
namespace {

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

// 找到与 open_pos 处开括号配对的闭括号，忽略字符串内的括号。
std::size_t FindClosingBracket(const std::string& text, std::size_t open_pos,
                               char open, char close) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t pos = open_pos; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == open) {
            ++depth;
        } else if (ch == close) {
            --depth;
            if (depth == 0) return pos;
        }
    }
    return std::string::npos;
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

bool EndsWithInsensitive(const std::wstring& text, const std::wstring& suffix) {
    if (suffix.size() > text.size()) return false;
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        wchar_t left = text[offset + i];
        wchar_t right = suffix[i];
        if (left >= L'A' && left <= L'Z') left = left - L'A' + L'a';
        if (right >= L'A' && right <= L'Z') right = right - L'A' + L'a';
        if (left != right) return false;
    }
    return true;
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

bool ParseReleaseAssets(const std::string& json,
                        std::vector<ReleaseAsset>* assets) {
    if (!assets) return false;
    assets->clear();

    const std::string key = "\"assets\"";
    std::size_t pos = json.find(key);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + key.size());
    if (pos == std::string::npos) return false;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return false;

    const std::size_t array_end = FindClosingBracket(json, pos, '[', ']');
    if (array_end == std::string::npos) return false;

    std::size_t cursor = pos + 1;
    while (cursor < array_end) {
        const std::size_t open = json.find('{', cursor);
        if (open == std::string::npos || open >= array_end) break;
        const std::size_t close = FindClosingBracket(json, open, '{', '}');
        if (close == std::string::npos || close > array_end) break;

        const std::string object = json.substr(open, close - open + 1);
        std::string name_utf8;
        std::string url_utf8;
        if (FindJsonString(object, "name", &name_utf8) &&
            FindJsonString(object, "browser_download_url", &url_utf8)) {
            ReleaseAsset asset;
            asset.name = Utf8ToWide(name_utf8);
            asset.download_url = Utf8ToWide(url_utf8);
            if (!asset.name.empty() && IsWebUrl(asset.download_url)) {
                assets->push_back(std::move(asset));
            }
        }
        cursor = close + 1;
    }
    return !assets->empty();
}

ReleaseFetchStatus FetchLatestRelease(LatestRelease* release,
                                      std::wstring* error,
                                      int timeout_ms) {
    if (!release) return ReleaseFetchStatus::NetworkError;
    *release = LatestRelease{};

    std::string body;
    if (!http::GetText(kLatestReleaseApiUrl, timeout_ms, &body, error)) {
        return ReleaseFetchStatus::NetworkError;
    }

    LatestRelease parsed;
    if (!ParseLatestRelease(body, &parsed.tag, &parsed.html_url, error)) {
        return ReleaseFetchStatus::ParseError;
    }
    parsed.version = NormalizeVersion(parsed.tag);
    if (ParseNumericComponents(parsed.version).empty()) {
        if (error) *error = L"版本号格式无效";
        return ReleaseFetchStatus::ParseError;
    }
    ParseReleaseAssets(body, &parsed.assets);

    USB_LOG_INFO(L"update", L"最新 release：%s，资产 %llu 个",
                 parsed.version.c_str(),
                 static_cast<unsigned long long>(parsed.assets.size()));
    *release = std::move(parsed);
    return ReleaseFetchStatus::Ok;
}

std::wstring ReleaseAssetSuffix(const std::wstring& config_label,
                                const std::wstring& arch_label) {
    return L"-" + config_label + L"-" + arch_label + L".zip";
}

bool SelectReleaseAsset(const LatestRelease& release,
                        const std::wstring& config_label,
                        const std::wstring& arch_label, ReleaseAsset* asset) {
    if (!asset) return false;
    const std::wstring suffix = ReleaseAssetSuffix(config_label, arch_label);
    for (const ReleaseAsset& candidate : release.assets) {
        if (EndsWithInsensitive(candidate.name, suffix) &&
            IsWebUrl(candidate.download_url)) {
            *asset = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace usb
