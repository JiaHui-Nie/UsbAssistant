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

void ApplyPendingUpdaterUpdate() {
    const std::wstring directory = ExecutableDirectory();
    if (directory.empty()) return;

    const std::wstring pending =
        directory + L"\\" + kUpdaterPendingExeName;
    if (GetFileAttributesW(pending.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    const std::wstring target = directory + L"\\" + kUpdaterExeName;
    if (!MoveFileExW(pending.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        // 升级器正在运行等情况：保留 .new，下次启动再试。
        USB_LOG_WARN(L"app", L"升级器待生效文件替换失败：%lu", GetLastError());
        return;
    }
    USB_LOG_INFO(L"app", L"升级器已替换为新版本");
}

bool HasPendingUpdaterUpdate() {
    const std::wstring directory = ExecutableDirectory();
    if (directory.empty()) return false;
    const std::wstring pending =
        directory + L"\\" + kUpdaterPendingExeName;
    return GetFileAttributesW(pending.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace usb
