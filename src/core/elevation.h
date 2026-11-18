// 提权助手协议：主进程用 UAC 拉起同一个 exe 的 --helper 模式，
// 助手完成「解除占用 + 弹出」后把结果写进临时文件并退出。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace usb {

enum class HelperOperation {
    Eject = 0,
    Mount = 1,
    Unmount = 2,
    Open = 3,
};

struct HelperRequest {
    HelperOperation operation = HelperOperation::Eject;
    std::uint32_t disk_number = 0;
    std::vector<wchar_t> letters;
    std::wstring volume_path;  // Mount 使用
    wchar_t letter = 0;        // Unmount 使用
    std::wstring root;         // Open 使用，例如 "F:\"
    std::wstring result_path;
};

constexpr int kHelperCancelled = -2;
constexpr int kHelperNoResult = -1;

bool ParseHelperArguments(int argc, wchar_t* const* argv, HelperRequest* request);
std::wstring BuildHelperArguments(const HelperRequest& request);
std::wstring MakeHelperResultPath();

// 由 --helper 进程调用；返回退出码（EjectStatus / MountStatus）。
int RunHelper(const HelperRequest& request);

// 用当前令牌启动一个独立的资源管理器窗口（/separate）。
// 在提权助手进程里调用即可得到能浏览受保护卷的管理员窗口；
// Explorer 不会把它转交给中等完整性的桌面 Explorer 进程。
bool LaunchExplorerSeparate(const std::wstring& root);

struct HelperOutcome {
    int status = kHelperNoResult;
    wchar_t letter = 0;
    std::wstring message;
    bool cancelled = false;
};

// 启动提权助手并等待结果。必须从后台线程调用（会阻塞）。
HelperOutcome LaunchHelperAndWait(const std::wstring& executable,
                                  const HelperRequest& request,
                                  DWORD timeout_ms);

}  // namespace usb
