#include "core/elevation.h"

#include <shellapi.h>

#include <cwchar>
#include <string>
#include <vector>

#include "core/eject.h"
#include "core/log.h"
#include "core/occupants.h"
#include "core/storage.h"

namespace usb {
namespace {

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    // 结尾的反斜杠必须加倍，否则会把闭合引号转义掉。
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool WriteResultFile(const std::wstring& path, int status,
                     const std::wstring& message, wchar_t letter = 0) {
    if (path.empty()) return false;
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::wstring text = std::to_wstring(status) + L"\n";
    if (letter != 0) text += std::wstring(1, letter) + L"\n";
    text += message;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.c_str(),
                              static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                              &written, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

HelperOutcome ReadResultFile(const std::wstring& path) {
    HelperOutcome outcome;
    if (path.empty()) return outcome;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return outcome;

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        size.QuadPart < 64 * 1024) {
        std::wstring text(
            static_cast<std::size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
        DWORD read = 0;
        if (ReadFile(file, text.data(),
                     static_cast<DWORD>(text.size() * sizeof(wchar_t)), &read,
                     nullptr)) {
            text.resize(read / sizeof(wchar_t));
            const std::size_t newline = text.find(L'\n');
            if (newline != std::wstring::npos) {
                outcome.status = _wtoi(text.substr(0, newline).c_str());
                std::wstring rest = text.substr(newline + 1);
                const std::size_t second = rest.find(L'\n');
                if (second == 1 && rest[0] >= L'A' && rest[0] <= L'Z') {
                    outcome.letter = rest[0];
                    outcome.message = rest.substr(second + 1);
                } else {
                    outcome.message = rest;
                }
            }
        }
    }
    CloseHandle(file);
    return outcome;
}

}  // namespace

bool ParseHelperArguments(int argc, wchar_t* const* argv,
                          HelperRequest* request) {
    if (!request) return false;
    bool helper = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--helper") {
            helper = true;
        } else if (arg == L"--op" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"mount") {
                request->operation = HelperOperation::Mount;
            } else if (value == L"unmount") {
                request->operation = HelperOperation::Unmount;
            } else if (value == L"open") {
                request->operation = HelperOperation::Open;
            } else {
                request->operation = HelperOperation::Eject;
            }
        } else if (arg == L"--volume" && i + 1 < argc) {
            request->volume_path = argv[++i];
        } else if (arg == L"--root" && i + 1 < argc) {
            request->root = argv[++i];
        } else if (arg == L"--letter" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            request->letter =
                value.empty() ? 0 : static_cast<wchar_t>(value[0]);
        } else if (arg == L"--disk" && i + 1 < argc) {
            request->disk_number = static_cast<std::uint32_t>(_wtoi(argv[++i]));
        } else if (arg == L"--letters" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            request->letters.clear();
            if (value.rfind(L"--", 0) != 0) {
                for (wchar_t ch : value) {
                    if (ch != 0) request->letters.push_back(ch);
                }
            }
        } else if (arg == L"--result" && i + 1 < argc) {
            request->result_path = argv[++i];
        }
    }
    return helper;
}

std::wstring BuildHelperArguments(const HelperRequest& request) {
    std::wstring arguments;
    if (request.operation == HelperOperation::Mount) {
        arguments = L"--helper --op mount --volume " +
                    QuoteArgument(request.volume_path);
    } else if (request.operation == HelperOperation::Unmount) {
        arguments = L"--helper --op unmount --letter " +
                    std::wstring(1, request.letter);
    } else if (request.operation == HelperOperation::Open) {
        arguments = L"--helper --op open --root " +
                    QuoteArgument(request.root);
    } else {
        std::wstring letters(request.letters.begin(), request.letters.end());
        arguments = L"--helper --disk " +
                    std::to_wstring(request.disk_number) +
                    L" --letters " + QuoteArgument(letters);
    }
    if (!request.result_path.empty()) {
        arguments += L" --result " + QuoteArgument(request.result_path);
    }
    return arguments;
}

std::wstring MakeHelperResultPath() {
    wchar_t temp_directory[MAX_PATH] = {};
    wchar_t result_path[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temp_directory) ||
        !GetTempFileNameW(temp_directory, L"usb", 0, result_path)) {
        return L"";
    }
    DeleteFileW(result_path);  // 让助手自己创建，便于判断“没有结果”
    return result_path;
}

bool LaunchExplorerSeparate(const std::wstring& root) {
    if (root.empty()) return false;

    wchar_t windows_directory[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windows_directory, MAX_PATH) == 0) return false;
    const std::wstring explorer =
        std::wstring(windows_directory) + L"\\explorer.exe";

    // /separate 让 Explorer 起独立进程；从提权助手启动时它继承高完整性
    // 令牌，因此能读取访问控制只放行管理员的卷（例如 ESP）。
    const std::wstring parameters = L"/separate," + root;

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = explorer.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

int RunHelper(const HelperRequest& request) {
    USB_LOG_INFO(L"elevation",
                 L"helper 开始：op=%d disk=%u letter=%c volume=%s",
                 static_cast<int>(request.operation), request.disk_number,
                 request.letter ? request.letter : L'-',
                 request.volume_path.c_str());
    if (request.operation == HelperOperation::Open) {
        const bool ok = LaunchExplorerSeparate(request.root);
        USB_LOG_INFO(L"elevation", L"helper Open 结果=%d root=%s", ok ? 1 : 0,
                     request.root.c_str());
        WriteResultFile(request.result_path, ok ? 0 : 1,
                        ok ? L"" : L"无法启动资源管理器窗口。");
        return ok ? 0 : 1;
    }

    if (request.operation == HelperOperation::Mount) {
        // 已有盘符时直接复用，避免重复分配。
        wchar_t letter = storage::FindVolumeDriveLetter(request.volume_path);
        const storage::MountResult result =
            letter != 0
                ? storage::MountResult{storage::MountStatus::Ok, letter, L""}
                : storage::AssignDriveLetter(request.volume_path);
        USB_LOG_INFO(L"elevation",
                     L"helper Mount 结果：status=%d letter=%c message=%s",
                     static_cast<int>(result.status),
                     result.letter ? result.letter : L'-',
                     result.message.c_str());
        WriteResultFile(request.result_path, static_cast<int>(result.status),
                        result.message, result.ok() ? result.letter : 0);
        return static_cast<int>(result.status);
    }

    if (request.operation == HelperOperation::Unmount) {
        const storage::MountResult result =
            storage::RemoveDriveLetter(request.letter);
        USB_LOG_INFO(L"elevation",
                     L"helper Unmount 结果：status=%d letter=%c message=%s",
                     static_cast<int>(result.status),
                     request.letter ? request.letter : L'-',
                     result.message.c_str());
        WriteResultFile(request.result_path, static_cast<int>(result.status),
                        result.message);
        return static_cast<int>(result.status);
    }

    // 1. 以管理员权限解除占用。
    std::vector<Occupant> occupants = FindOccupants(request.letters);
    std::vector<DWORD> pids;
    pids.reserve(occupants.size());
    for (const auto& occupant : occupants) pids.push_back(occupant.pid);
    occupants.clear();

    if (!pids.empty()) {
        ReleaseOccupants(pids, request.letters);
        Sleep(300);
    }

    // 2. 执行弹出。
    const EjectResult result =
        EjectAfterRelease(request.disk_number, L"", request.letters);
    USB_LOG_INFO(L"elevation", L"helper Eject 结果：status=%d message=%s",
                 static_cast<int>(result.status), result.message.c_str());
    WriteResultFile(request.result_path, static_cast<int>(result.status),
                    result.message);
    return static_cast<int>(result.status);
}

HelperOutcome LaunchHelperAndWait(const std::wstring& executable,
                                  const HelperRequest& request,
                                  DWORD timeout_ms) {
    HelperOutcome outcome;

    const std::wstring arguments = BuildHelperArguments(request);
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC |
                 SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info) || !info.hProcess) {
        outcome.cancelled = GetLastError() == ERROR_CANCELLED;
        outcome.message = outcome.cancelled
                              ? L"未获得管理员权限，操作已取消。"
                              : L"无法启动管理员模式，操作未执行。";
        USB_LOG_WARN(L"elevation",
                     L"runas 启动失败：cancelled=%d error=0x%08lX args=%s",
                     outcome.cancelled ? 1 : 0,
                     static_cast<unsigned long>(GetLastError()),
                     arguments.c_str());
        return outcome;
    }
    USB_LOG_INFO(L"elevation", L"runas 启动成功：args=%s", arguments.c_str());

    DWORD exit_code = 99;
    if (WaitForSingleObject(info.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        GetExitCodeProcess(info.hProcess, &exit_code);
    } else {
        TerminateProcess(info.hProcess, 99);
        WaitForSingleObject(info.hProcess, 1000);
        outcome.message = L"管理员模式超时未返回结果，操作可能未执行。";
        USB_LOG_WARN(L"elevation", L"helper 超时（%lu ms）已终止",
                     static_cast<unsigned long>(timeout_ms));
    }
    CloseHandle(info.hProcess);
    USB_LOG_INFO(L"elevation", L"helper 退出码=%lu",
                 static_cast<unsigned long>(exit_code));

    HelperOutcome file_result = ReadResultFile(request.result_path);
    DeleteFileW(request.result_path.c_str());
    if (file_result.status >= 0) {
        USB_LOG_INFO(L"elevation",
                     L"helper 结果文件：status=%d letter=%c message=%s",
                     file_result.status,
                     file_result.letter ? file_result.letter : L'-',
                     file_result.message.c_str());
        return file_result;
    }
    USB_LOG_WARN(L"elevation", L"helper 没有可用的结果文件");

    outcome.status =
        exit_code <= 16 ? static_cast<int>(exit_code) : kHelperNoResult;
    return outcome;
}

}  // namespace usb
