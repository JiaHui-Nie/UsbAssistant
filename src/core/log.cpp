#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/settings.h"

namespace usb::log {
namespace {

constexpr std::uint64_t kMaxFileBytes = 2ull * 1024ull * 1024ull;
constexpr int kMaxBackups = 3;
constexpr std::size_t kMaxQueue = 4096;

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::wstring> queue;
    std::thread writer;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring directory;
    std::wstring path;
    std::uint64_t size = 0;
    bool started = false;
    unsigned dropped = 0;
    std::atomic<bool> enabled{false};
};

State& Store() {
    static State state;
    return state;
}

constexpr bool DefaultEnabled() {
#ifdef _DEBUG
    return true;
#else
    return false;
#endif
}

bool EnvironmentEnabled(bool fallback) {
    wchar_t value[16] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"USBASSISTANT_LOG", value, 16);
    if (length == 0 || length >= 16) return fallback;
    if (value[0] == L'0' || value[0] == L'f' || value[0] == L'F') return false;
    if (value[0] == L'1' || value[0] == L't' || value[0] == L'T') return true;
    return fallback;
}

wchar_t LevelChar(Level level) {
    switch (level) {
        case Level::Debug:
            return L'D';
        case Level::Info:
            return L'I';
        case Level::Warn:
            return L'W';
        case Level::Error:
            return L'E';
    }
    return L'I';
}

std::wstring BackupPath(const std::wstring& directory, int index) {
    return directory + L"\\usbassistant." + std::to_wstring(index) + L".log";
}

// 以下两个函数都在持有 mutex 的情况下调用。
bool OpenFileLocked() {
    State& state = Store();
    if (state.file != INVALID_HANDLE_VALUE) return true;
    if (state.path.empty()) return false;

    state.file = CreateFileW(state.path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (state.file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(state.file, &size)) {
        state.size = static_cast<std::uint64_t>(size.QuadPart);
    } else {
        state.size = 0;
    }
    if (state.size == 0) {
        static constexpr unsigned char kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        WriteFile(state.file, kUtf8Bom, sizeof(kUtf8Bom), &written, nullptr);
        state.size = written;
    }
    return true;
}

void RotateLocked() {
    State& state = Store();
    if (state.file != INVALID_HANDLE_VALUE) {
        CloseHandle(state.file);
        state.file = INVALID_HANDLE_VALUE;
    }
    state.size = 0;

    DeleteFileW(BackupPath(state.directory, kMaxBackups).c_str());
    for (int i = kMaxBackups - 1; i >= 1; --i) {
        MoveFileExW(BackupPath(state.directory, i).c_str(),
                    BackupPath(state.directory, i + 1).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
    MoveFileExW(state.path.c_str(),
                BackupPath(state.directory, 1).c_str(),
                MOVEFILE_REPLACE_EXISTING);
    OpenFileLocked();
}

int Utf8Length(const std::wstring& text) {
    return WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                               static_cast<int>(text.size()), nullptr, 0,
                               nullptr, nullptr);
}

// 要求 state.file 有效且持有 mutex。
void WriteUtf8Locked(const std::wstring& text) {
    State& state = Store();
    if (state.file == INVALID_HANDLE_VALUE) return;
    const int needed = Utf8Length(text);
    if (needed <= 0) return;
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), utf8.data(), needed,
                        nullptr, nullptr);
    DWORD written = 0;
    if (WriteFile(state.file, utf8.data(), static_cast<DWORD>(utf8.size()),
                  &written, nullptr)) {
        state.size += written;
    }
}

void WriteBatch(std::deque<std::wstring>* batch) {
    State& state = Store();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file == INVALID_HANDLE_VALUE && !OpenFileLocked()) return;

    for (const std::wstring& line : *batch) {
        const int needed = Utf8Length(line);
        if (needed > 0 &&
            state.size + static_cast<std::uint64_t>(needed) > kMaxFileBytes) {
            RotateLocked();
            // 滚动后的新文件同样在开头写明实际路径，避免用户拿错文件。
            WriteUtf8Locked(L"=== 日志滚动，实际路径：" + state.path +
                            L" ===\r\n");
        }
        WriteUtf8Locked(line);
        if (state.file == INVALID_HANDLE_VALUE) return;
    }
    if (state.file != INVALID_HANDLE_VALUE) FlushFileBuffers(state.file);
}

void WriterLoop() {
    State& state = Store();
    std::unique_lock<std::mutex> lock(state.mutex);
    while (true) {
        state.cv.wait_for(lock, std::chrono::milliseconds(400), [] {
            State& current = Store();
            return !current.enabled.load() || !current.queue.empty();
        });
        if (!state.enabled.load() && state.queue.empty()) break;

        std::deque<std::wstring> batch;
        batch.swap(state.queue);
        lock.unlock();
        WriteBatch(&batch);
        lock.lock();
    }
    if (state.file != INVALID_HANDLE_VALUE) {
        CloseHandle(state.file);
        state.file = INVALID_HANDLE_VALUE;
    }
}

bool IsPortableDirectory(const std::wstring& directory) {
    wchar_t module[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return true;
    std::wstring path = module;
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    path.resize(slash);
    return _wcsicmp(path.c_str(), directory.c_str()) == 0;
}

}  // namespace

bool Enabled() { return Store().enabled.load(); }

void Init() {
    State& state = Store();
    std::wstring location;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.started) return;
        state.started = true;
        if (!EnvironmentEnabled(DefaultEnabled())) return;

        state.directory = ::usb::DataDirectory();
        state.path = state.directory + L"\\usbassistant.log";
        if (!OpenFileLocked()) return;
        state.enabled.store(true);
        state.writer = std::thread(WriterLoop);
        location = IsPortableDirectory(state.directory)
                       ? L"软件目录"
                       : L"软件目录不可写，已回退到 %APPDATA%\\UsbAssistant";
    }
    USB_LOG_INFO(L"log", L"=== U 盘助手诊断日志 === 实际路径：%s（%s）",
                 state.path.c_str(), location.c_str());
    USB_LOG_INFO(L"log", L"日志滚动上限：2 MB，保留 3 份备份");
}

void SetEnabled(bool enabled) {
    State& state = Store();
    if (enabled) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.enabled.load()) return;
            if (state.path.empty()) {
                state.directory = ::usb::DataDirectory();
                state.path = state.directory + L"\\usbassistant.log";
            }
            if (!OpenFileLocked()) return;
            state.enabled.store(true);
            if (!state.writer.joinable()) {
                state.writer = std::thread(WriterLoop);
            }
        }
        USB_LOG_INFO(L"log", L"日志已重新开启：%s", state.path.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.enabled.exchange(false)) return;
    }
    state.cv.notify_all();
    if (state.writer.joinable()) state.writer.join();
}

void Shutdown() {
    State& state = Store();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.started) return;
        state.enabled.store(false);
    }
    state.cv.notify_all();
    if (state.writer.joinable()) state.writer.join();
}

void Write(Level level, const wchar_t* module, const std::wstring& message) {
    State& state = Store();
    if (!state.enabled.load()) return;

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t stamp[40] = {};
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond, time.wMilliseconds);

    std::wstring line;
    line.reserve(message.size() + 64);
    line += stamp;
    line += L" [";
    line += std::to_wstring(GetCurrentThreadId());
    line += L"] ";
    line += LevelChar(level);
    line += L" ";
    line += module ? module : L"?";
    line += L": ";
    line += message;
    line += L"\r\n";

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.enabled.load()) return;
        if (state.queue.size() >= kMaxQueue) {
            ++state.dropped;
            return;
        }
        state.queue.push_back(std::move(line));
    }
    state.cv.notify_one();
}

std::wstring Format(const wchar_t* format, ...) {
    if (!format) return L"";
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int needed = _vscwprintf(format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return format;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    _vsnwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, format, args);
    va_end(args);
    return std::wstring(buffer.data(), static_cast<std::size_t>(needed));
}

}  // namespace usb::log
