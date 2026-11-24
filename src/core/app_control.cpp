#include "core/app_control.h"

#include <algorithm>

#include "core/log.h"

namespace usb {

std::wstring ExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(),
                           static_cast<DWORD>(buffer.size()));
    if (length == 0) return L"";
    buffer.resize(std::min<std::size_t>(length, buffer.size() - 1));
    return buffer;
}

std::wstring ExecutableDirectory() {
    std::wstring path = ExecutablePath();
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    path.resize(slash);
    return path;
}

RunningApp FindRunningApp() {
    RunningApp app;
    const HWND window = FindWindowW(kMainWindowClass, nullptr);
    if (!window) return app;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) return app;
    app.window = window;
    app.pid = pid;
    return app;
}

StopAppResult RequestAppExit(const RunningApp& app, int grace_ms,
                             std::wstring* error) {
    if (!app.window || app.pid == 0) return StopAppResult::NotRunning;
    if (!IsWindow(app.window)) return StopAppResult::NotRunning;

    const UINT message = RegisterWindowMessageW(kExitRequestMessageName);
    if (message == 0) {
        USB_LOG_WARN(L"update", L"无法注册退出请求消息");
    } else if (!PostMessageW(app.window, message, 0, 0)) {
        USB_LOG_WARN(L"update", L"发送退出请求失败：%lu", GetLastError());
    }

    HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, app.pid);
    if (!process) {
        // 打不开进程（通常是权限差异）：只能等窗口消失，不能强杀。
        for (int elapsed = 0; elapsed < grace_ms; elapsed += 100) {
            if (!IsWindow(app.window)) return StopAppResult::Exited;
            Sleep(100);
        }
        if (error) *error = L"无法结束正在运行的程序";
        return StopAppResult::Failed;
    }

    StopAppResult result = StopAppResult::Failed;
    if (WaitForSingleObject(process, static_cast<DWORD>(std::max(grace_ms, 0))) ==
        WAIT_OBJECT_0) {
        result = StopAppResult::Exited;
    } else if (TerminateProcess(process, 0)) {
        if (WaitForSingleObject(process, 2000) == WAIT_OBJECT_0) {
            result = StopAppResult::Terminated;
            USB_LOG_WARN(L"update", L"主程序未响应退出请求，已强制结束");
        }
    }
    CloseHandle(process);

    if (result == StopAppResult::Failed && error) {
        *error = L"无法结束正在运行的程序";
    }
    return result;
}

bool LaunchApp(const std::wstring& exe_path, const std::wstring& arguments,
               const std::wstring& working_dir) {
    if (exe_path.empty()) return false;

    std::wstring command = L"\"" + exe_path + L"\"";
    if (!arguments.empty()) {
        command += L" ";
        command += arguments;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    const BOOL ok = CreateProcessW(
        exe_path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(), &startup, &info);
    if (info.hThread) CloseHandle(info.hThread);
    if (info.hProcess) CloseHandle(info.hProcess);
    if (!ok) {
        USB_LOG_ERROR(L"update", L"启动失败：%s（%lu）", exe_path.c_str(),
                      GetLastError());
    }
    return ok != FALSE;
}

bool DeleteUpdaterLeftover() {
    const std::wstring directory = ExecutableDirectory();
    if (directory.empty()) return true;

    // 旧版本曾把新升级器落成 .new 交给主程序改名；这里顺手清掉残留。
    const std::wstring legacy_pending =
        directory + L"\\" + kUpdaterLegacyPendingExeName;
    SetFileAttributesW(legacy_pending.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(legacy_pending.c_str());

    const std::wstring leftover = directory + L"\\" + kUpdaterOldExeName;
    if (GetFileAttributesW(leftover.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    // 上一次自我替换时旧进程可能还没完全退出，文件短暂被占用。
    for (int attempt = 0; attempt < 10; ++attempt) {
        SetFileAttributesW(leftover.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(leftover.c_str())) {
            USB_LOG_INFO(L"update", L"已删除旧版升级器备份");
            return true;
        }
        Sleep(100);
    }
    USB_LOG_WARN(L"update", L"删除旧版升级器备份失败：%lu", GetLastError());
    return false;
}

}  // namespace usb
