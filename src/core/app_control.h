// 主程序进程控制：查找运行实例、请求优雅退出、兜底强制结束与重新启动。
#pragma once

#include <windows.h>

#include <string>

namespace usb {

inline constexpr wchar_t kMainWindowClass[] = L"UsbAssistantMain";
inline constexpr wchar_t kMainExeName[] = L"UsbAssistant.exe";
inline constexpr wchar_t kUpdaterExeName[] = L"UsbAssistantUpdater.exe";
// 升级器解压出的待生效文件；主程序启动早期把它换成正式文件名。
inline constexpr wchar_t kUpdaterPendingExeName[] =
    L"UsbAssistantUpdater.new.exe";

// 升级器通知主程序退出用的注册消息名，两边必须一致。
inline constexpr wchar_t kExitRequestMessageName[] =
    L"UsbAssistant.ExitRequest.1";

// 当前进程可执行文件的完整路径与所在目录。
std::wstring ExecutablePath();
std::wstring ExecutableDirectory();

struct RunningApp {
    HWND window = nullptr;
    DWORD pid = 0;
};

// 查找正在运行的主程序（未运行时 window 为空）。
RunningApp FindRunningApp();

enum class StopAppResult {
    NotRunning,
    Exited,      // 优雅退出成功
    Terminated,  // 超时后强制结束
    Failed,      // 无法结束
};

// 先向主程序发送退出请求并等待 grace_ms；超时后 TerminateProcess 兜底。
StopAppResult RequestAppExit(const RunningApp& app, int grace_ms,
                             std::wstring* error);

// 启动指定程序；arguments 为已拼好的命令行参数（可为空）。
bool LaunchApp(const std::wstring& exe_path, const std::wstring& arguments,
               const std::wstring& working_dir);

// 主程序启动早期调用：把升级器留下的 UsbAssistantUpdater.new.exe 替换为
// UsbAssistantUpdater.exe。成功才删除 .new；失败保留文件，下次启动再试。
void ApplyPendingUpdaterUpdate();

// 是否还有待生效的升级器文件（用于启动后再补一次替换）。
bool HasPendingUpdaterUpdate();

}  // namespace usb
