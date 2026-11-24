// 主程序进程控制：查找运行实例、请求优雅退出、兜底强制结束与重新启动。
#pragma once

#include <windows.h>

#include <string>

namespace usb {

inline constexpr wchar_t kMainWindowClass[] = L"UsbAssistantMain";
inline constexpr wchar_t kMainExeName[] = L"UsbAssistant.exe";
inline constexpr wchar_t kUpdaterExeName[] = L"UsbAssistantUpdater.exe";
// 升级器自我替换时，正在运行的本体会先改名为 .old；新版本直接写成正式
// 文件名，由下一次升级器启动时删除 .old。
inline constexpr wchar_t kUpdaterOldExeName[] =
    L"UsbAssistantUpdater.old.exe";
// 旧版本使用的待生效文件名，只在清理历史残留时出现。
inline constexpr wchar_t kUpdaterLegacyPendingExeName[] =
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

// 升级器启动时调用：删除上一次自我替换留下的 .old 文件。
// 旧进程可能还在退出，会短暂重试；仍未成功就留给下一次启动。
bool DeleteUpdaterLeftover();

}  // namespace usb
