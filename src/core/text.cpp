#include "core/text.h"

#include <windows.h>
#include <winver.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace usb::text {
namespace {

bool IsGenericToken(const std::wstring& token) {
    // 以 usb / generic 开头的词一律视为通用词（USB、USB2.0、GenericFlash…）。
    if (StartsWithInsensitive(token, L"usb") ||
        StartsWithInsensitive(token, L"generic")) {
        return true;
    }
    static const wchar_t* kWords[] = {
        L"mass", L"storage", L"disk",   L"device", L"removable",
        L"drive", L"2.0", L"3.0", L"3.1", L"3.2",
    };
    for (const wchar_t* word : kWords) {
        if (IEquals(token, word)) return true;
    }
    return false;
}

bool IsTrailingNoise(const std::wstring& token) {
    return IEquals(token, L"device") || IEquals(token, L"usb") ||
           IEquals(token, L"设备");
}

std::wstring StripLeadingGeneric(const std::wstring& value) {
    const auto tokens = SplitWhitespace(value);
    std::size_t begin = 0;
    while (begin < tokens.size() && IsGenericToken(tokens[begin])) ++begin;
    return Join(tokens, begin, tokens.size());
}

}  // namespace

std::wstring Trim(std::wstring value) {
    const auto not_space = [](wchar_t ch) {
        return ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n';
    };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::wstring ToLower(std::wstring value) {
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

bool IEquals(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool StartsWithInsensitive(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.size() > text.size()) return false;
    return _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle) {
    if (needle.empty()) return true;
    return ToLower(text).find(ToLower(needle)) != std::wstring::npos;
}

std::vector<std::wstring> SplitWhitespace(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::wstring Join(const std::vector<std::wstring>& tokens, std::size_t begin,
                  std::size_t end) {
    if (begin > tokens.size()) begin = tokens.size();
    if (end > tokens.size()) end = tokens.size();
    std::wstring result;
    for (std::size_t i = begin; i < end; ++i) {
        if (!result.empty()) result += L' ';
        result += tokens[i];
    }
    return result;
}

std::wstring CleanPnpName(const std::wstring& value) {
    const auto tokens = SplitWhitespace(value);
    std::size_t begin = 0;
    while (begin < tokens.size() && IsGenericToken(tokens[begin])) ++begin;
    std::size_t end = tokens.size();
    while (end > begin && IsTrailingNoise(tokens[end - 1])) --end;
    return Join(tokens, begin, end);
}

std::wstring DeviceTitle(const std::wstring& vendor, const std::wstring& product,
                         bool removable_media) {
    const std::wstring clean_vendor = StripLeadingGeneric(vendor);
    const std::wstring clean_product = StripLeadingGeneric(product);

    std::wstring title;
    if (!clean_vendor.empty() && !clean_product.empty()) {
        title = clean_vendor + L" " + clean_product;
    } else if (!clean_product.empty()) {
        title = clean_product;
    } else if (!clean_vendor.empty()) {
        title = clean_vendor;
    }
    if (title.empty()) {
        title = removable_media ? L"可移动磁盘" : L"移动硬盘";
    }
    return title;
}

std::wstring FormatBytes(std::uint64_t bytes, int capacity_base) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    const double base = capacity_base == 1000 ? 1000.0 : 1024.0;
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= base && unit < 5) {
        value /= base;
        ++unit;
    }

    wchar_t buffer[64] = {};
    if (unit == 0) {
        swprintf(buffer, 64, L"%llu", static_cast<unsigned long long>(bytes));
    } else {
        swprintf(buffer, 64, L"%.2f %ls", value, kUnits[unit]);
        return buffer;
    }
    std::wstring result = buffer;
    result += L" ";
    result += kUnits[unit];
    return result;
}

bool IsOpenableFileSystem(const std::wstring& filesystem) {
    static const wchar_t* kSupported[] = {L"NTFS", L"FAT", L"FAT32", L"exFAT",
                                          L"ReFS"};
    for (const wchar_t* name : kSupported) {
        if (IEquals(filesystem, name)) return true;
    }
    return false;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return L"";

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";

    struct Translation {
        WORD language;
        WORD code_page;
    };
    Translation* translation = nullptr;
    UINT translation_size = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translation),
                        &translation_size) ||
        translation == nullptr ||
        translation_size < sizeof(Translation)) {
        return L"";
    }

    wchar_t tag[16] = {};
    swprintf(tag, 16, L"%04x%04x", translation->language, translation->code_page);
    std::wstring key = L"\\StringFileInfo\\";
    key += tag;
    key += L"\\FileDescription";

    wchar_t* value = nullptr;
    UINT value_size = 0;
    if (VerQueryValueW(data.data(), key.c_str(),
                       reinterpret_cast<void**>(&value), &value_size) &&
        value != nullptr && value_size > 1) {
        return Trim(value);
    }
    return L"";
}

std::wstring BaseNameWithoutExtension(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    std::wstring name =
        slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

}  // namespace usb::text
