#include "core/update_schedule.h"

#include <windows.h>

#include <cstdlib>
#include <iterator>
#include <string>

#include "core/settings.h"

namespace usb {
namespace {

constexpr wchar_t kSection[] = L"Update";
constexpr wchar_t kLastCheckKey[] = L"LastCheck";

std::wstring StateFilePath() { return DataDirectory() + L"\\config.ini"; }

}  // namespace

std::int64_t UpdateIntervalSeconds(int frequency) {
    switch (frequency) {
        case kUpdateHourly:
            return 60 * 60;
        case kUpdateDaily:
            return 24 * 60 * 60;
        case kUpdateWeekly:
            return 7 * 24 * 60 * 60;
        case kUpdateMonthly:
            return 30 * 24 * 60 * 60;
        case kUpdateOnStartup:
        default:
            return 0;
    }
}

bool UpdateCheckDue(int frequency, std::int64_t last_check,
                    std::int64_t now) {
    const std::int64_t interval = UpdateIntervalSeconds(frequency);
    if (interval <= 0) return true;
    if (last_check <= 0) return true;
    // 系统时间被回拨时按“还没到期”处理，等时钟追上再检查。
    if (now <= last_check) return false;
    return now - last_check >= interval;
}

std::int64_t NowSeconds() {
    FILETIME file_time = {};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    // FILETIME 是 1601-01-01 起的 100ns；换成 1970-01-01 起的秒。
    constexpr unsigned long long kEpochDifference = 116444736000000000ull;
    if (value.QuadPart < kEpochDifference) return 0;
    return static_cast<std::int64_t>((value.QuadPart - kEpochDifference) /
                                     10000000ull);
}

std::int64_t ReadLastUpdateCheck() {
    const std::wstring path = StateFilePath();
    wchar_t buffer[64] = {};
    const DWORD length = GetPrivateProfileStringW(
        kSection, kLastCheckKey, L"", buffer,
        static_cast<DWORD>(std::size(buffer)), path.c_str());
    if (length == 0) return 0;

    wchar_t* end = nullptr;
    const long long value = _wcstoi64(buffer, &end, 10);
    if (end == buffer || value < 0) return 0;
    return static_cast<std::int64_t>(value);
}

void RecordLastUpdateCheck(std::int64_t now) {
    if (now < 0) now = 0;
    const std::wstring path = StateFilePath();
    const std::wstring text = std::to_wstring(now);
    WritePrivateProfileStringW(kSection, kLastCheckKey, text.c_str(),
                               path.c_str());
}

}  // namespace usb
