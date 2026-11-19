#include "core/occupants.h"

#include <windows.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winternl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/storage.h"
#include "core/text.h"

namespace usb {
namespace {

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);
using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);

constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr ULONG kObjectNameInformation = 1;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004u);

NtQuerySystemInformationFn g_query_system_information = nullptr;
NtQueryObjectFn g_query_object = nullptr;

void ResolveNtdll() {
    if (g_query_system_information && g_query_object) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    g_query_system_information =
        reinterpret_cast<NtQuerySystemInformationFn>(
            reinterpret_cast<void*>(
                GetProcAddress(ntdll, "NtQuerySystemInformation")));
    g_query_object = reinterpret_cast<NtQueryObjectFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject")));
}

// SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX 的公开布局。
struct HandleEntry {
    void* object;
    ULONG_PTR pid;
    ULONG_PTR handle_value;
    ULONG granted_access;
    USHORT creator_backtrace_index;
    USHORT object_type_index;
    ULONG handle_attributes;
    ULONG reserved;
};

struct HandleSnapshot {
    ULONG_PTR count;
    ULONG_PTR reserved;
    HandleEntry entries[1];
};

HANDLE ProcessHandle(DWORD pid, std::map<DWORD, HANDLE>* cache) {
    auto it = cache->find(pid);
    if (it != cache->end()) return it->second;
    HANDLE handle =
        OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE, pid);
    cache->emplace(pid, handle);
    return handle;
}

ULONG_PTR FindFileTypeIndex(const HandleSnapshot* snapshot, HANDLE probe) {
    if (!probe || probe == INVALID_HANDLE_VALUE) return 0;
    const ULONG_PTR self = static_cast<ULONG_PTR>(GetCurrentProcessId());
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(probe);
    for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
        if (snapshot->entries[i].pid == self &&
            snapshot->entries[i].handle_value == value) {
            return snapshot->entries[i].object_type_index;
        }
    }
    return 0;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid = {};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES privileges = {};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &privileges,
                                   sizeof(privileges), nullptr, nullptr) &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

std::wstring ProcessImageName(DWORD pid) {
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";
    wchar_t path[1024] = {};
    DWORD size = 1024;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    return ok ? std::wstring(path, size) : L"";
}

bool IsCriticalImage(const std::wstring& lower_base) {
    static const wchar_t* kNames[] = {
        L"system",   L"smss",  L"csrss",  L"wininit", L"winlogon",
        L"services", L"lsass", L"lsaiso", L"svchost", L"dwm",
        L"fontdrvhost",
    };
    for (const wchar_t* name : kNames) {
        if (lower_base == name) return true;
    }
    return false;
}

HICON LoadShellIcon(const std::wstring& path) {
    SHFILEINFOW info = {};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_LARGEICON) &&
        info.hIcon) {
        return info.hIcon;
    }
    return nullptr;
}

bool MatchesDevice(const std::wstring& lower_name,
                   const std::vector<std::wstring>& device_paths) {
    for (const auto& device : device_paths) {
        if (lower_name.size() < device.size()) continue;
        if (lower_name.compare(0, device.size(), device) != 0) continue;
        if (lower_name.size() == device.size() ||
            lower_name[device.size()] == L'\\') {
            return true;
        }
    }
    return false;
}

bool MatchesVolumeUrl(const std::wstring& url,
                      const std::vector<wchar_t>& letters) {
    constexpr wchar_t kPrefix[] = L"file:///";
    constexpr std::size_t kPrefixLength = 8;
    if (url.size() < kPrefixLength + 2) return false;
    if (_wcsnicmp(url.c_str(), kPrefix, kPrefixLength) != 0) return false;
    const wchar_t drive = static_cast<wchar_t>(towupper(url[kPrefixLength]));
    if (url[kPrefixLength + 1] != L':') return false;
    for (wchar_t letter : letters) {
        if (letter != 0 && static_cast<wchar_t>(towupper(letter)) == drive) {
            return true;
        }
    }
    return false;
}

int CloseExplorerWindows(const std::vector<wchar_t>& letters) {
    if (letters.empty()) return 0;

    const HRESULT init =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return 0;

    IShellWindows* shell_windows = nullptr;
    int closed = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                   IID_IShellWindows,
                                   reinterpret_cast<void**>(&shell_windows))) &&
        shell_windows) {
        long count = 0;
        shell_windows->get_Count(&count);
        for (long i = 0; i < count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            IDispatch* dispatch = nullptr;
            if (FAILED(shell_windows->Item(index, &dispatch)) || !dispatch) {
                continue;
            }

            IWebBrowser2* browser = nullptr;
            if (SUCCEEDED(dispatch->QueryInterface(
                    IID_IWebBrowser2, reinterpret_cast<void**>(&browser))) &&
                browser) {
                LONG_PTR hwnd = 0;
                browser->get_HWND(&hwnd);
                wchar_t class_name[64] = {};
                const bool is_folder =
                    hwnd != 0 &&
                    GetClassNameW(reinterpret_cast<HWND>(hwnd), class_name, 64) &&
                    _wcsicmp(class_name, L"CabinetWClass") == 0;
                BSTR url = nullptr;
                if (is_folder &&
                    SUCCEEDED(browser->get_LocationURL(&url)) && url) {
                    if (MatchesVolumeUrl(url, letters)) {
                        if (SUCCEEDED(browser->Quit())) ++closed;
                    }
                    SysFreeString(url);
                }
                browser->Release();
            }
            dispatch->Release();
        }
        shell_windows->Release();
    }

    if (uninit) CoUninitialize();
    return closed;
}

// 句柄对象名可能带完整文件路径；日志只保留到卷这一级，避免泄露用户数据。
std::wstring VolumePrefix(const std::wstring& object) {
    const std::size_t first = object.find(L'\\');
    if (first == std::wstring::npos) return object;
    const std::size_t second = object.find(L'\\', first + 1);
    if (second == std::wstring::npos) return object;
    const std::size_t third = object.find(L'\\', second + 1);
    return third == std::wstring::npos ? object : object.substr(0, third);
}

}  // namespace

std::vector<Occupant> FindOccupants(const std::vector<wchar_t>& letters,
                                    bool* complete) {
    if (complete) *complete = false;
    ResolveNtdll();
    if (!g_query_system_information || !g_query_object) {
        USB_LOG_WARN(L"occupants", L"句柄扫描不可用（ntdll 查询函数缺失）");
        return {};
    }

    const std::vector<std::wstring> device_paths =
        storage::DosDevicePaths(letters);
    if (device_paths.empty()) {
        USB_LOG_WARN(L"occupants", L"句柄扫描：没有可匹配的卷设备路径");
        return {};
    }
    USB_LOG_INFO(L"occupants", L"句柄扫描开始：盘符数=%zu 设备路径数=%zu",
                 letters.size(), device_paths.size());

    // 打开一个自身的临时文件，用于在句柄表里确认 File 对象的类型索引。
    wchar_t temp_directory[MAX_PATH] = {};
    wchar_t probe_path[MAX_PATH] = {};
    HANDLE probe = INVALID_HANDLE_VALUE;
    if (GetTempPathW(MAX_PATH, temp_directory) &&
        GetTempFileNameW(temp_directory, L"usb", 0, probe_path)) {
        probe = CreateFileW(probe_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                            nullptr);
        if (probe == INVALID_HANDLE_VALUE) DeleteFileW(probe_path);
    }

    ULONG size = 1u << 20;
    std::vector<BYTE> buffer;
    LONG status = 0;
    ULONG returned = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        buffer.resize(size);
        returned = 0;
        status = g_query_system_information(kSystemExtendedHandleInformation,
                                            buffer.data(), size, &returned);
        if (status == kStatusInfoLengthMismatch) {
            size = returned + 65536;
            continue;
        }
        size = returned;
        break;
    }

    ULONG_PTR file_type_index = 0;
    if (status == 0 && size != 0) {
        file_type_index = FindFileTypeIndex(
            reinterpret_cast<HandleSnapshot*>(buffer.data()), probe);
    }
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    if (status != 0 || size == 0) {
        USB_LOG_WARN(L"occupants",
                     L"句柄快照失败：status=0x%08lX size=%lu",
                     static_cast<unsigned long>(status),
                     static_cast<unsigned long>(size));
        return {};
    }

    auto* snapshot = reinterpret_cast<HandleSnapshot*>(buffer.data());
    USB_LOG_DEBUG(L"occupants", L"句柄快照条目=%llu",
                  static_cast<unsigned long long>(snapshot->count));
    const DWORD self = GetCurrentProcessId();
    std::map<DWORD, HANDLE> process_cache;
    std::set<DWORD> matched;

    for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
        const HandleEntry& entry = snapshot->entries[i];
        if (file_type_index != 0 && entry.object_type_index != file_type_index) {
            continue;
        }

        const DWORD pid = static_cast<DWORD>(entry.pid);
        if (pid == 0 || pid == 4 || pid == self) continue;
        if (matched.count(pid) != 0) continue;

        HANDLE process = ProcessHandle(pid, &process_cache);
        if (!process) continue;

        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process,
                             reinterpret_cast<HANDLE>(entry.handle_value),
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }
        if (GetFileType(duplicate) != FILE_TYPE_DISK) {
            CloseHandle(duplicate);
            continue;
        }

        wchar_t name_buffer[4096] = {};
        ULONG name_length = 0;
        if (g_query_object(duplicate, kObjectNameInformation, name_buffer,
                           sizeof(name_buffer) - 1, &name_length) == 0 &&
            name_length > 0) {
            auto* name = reinterpret_cast<UNICODE_STRING*>(name_buffer);
            if (name->Buffer && name->Length > 0) {
                const std::wstring lower = text::ToLower(std::wstring(
                    name->Buffer, name->Length / sizeof(wchar_t)));
                if (MatchesDevice(lower, device_paths)) {
                    matched.insert(pid);
                    USB_LOG_INFO(L"occupants",
                                 L"匹配占用的卷：pid=%lu volume=%s",
                                 static_cast<unsigned long>(pid),
                                 VolumePrefix(lower).c_str());
                }
            }
        }
        CloseHandle(duplicate);
    }

    for (auto& item : process_cache) {
        if (item.second) CloseHandle(item.second);
    }

    std::vector<Occupant> occupants;
    for (DWORD pid : matched) {
        const std::wstring path = ProcessImageName(pid);
        if (path.empty()) continue;

        Occupant occupant;
        occupant.pid = pid;
        occupant.path = path;
        occupant.name = text::FileDescription(path);
        if (occupant.name.empty()) {
            occupant.name = text::BaseNameWithoutExtension(path);
        }
        occupant.icon = LoadShellIcon(path);
        USB_LOG_INFO(L"occupants", L"占用进程：pid=%lu image=%s",
                     static_cast<unsigned long>(pid), path.c_str());
        occupants.push_back(std::move(occupant));
    }

    USB_LOG_INFO(L"occupants", L"句柄扫描完成：占用进程数=%zu",
                 occupants.size());
    if (complete) *complete = true;

    std::sort(occupants.begin(), occupants.end(),
              [](const Occupant& left, const Occupant& right) {
                  return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
              });
    return occupants;
}

bool OccupantsStillAlive(const std::vector<Occupant>& occupants) {
    for (const auto& occupant : occupants) {
        if (occupant.pid == 0) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE, occupant.pid);
        if (!process) {
            // 打不开不代表已退出（受保护进程会拒绝访问），保守视为存活；
            // 但参数无效/找不到说明 PID 已经不存在。
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
                return false;
            }
            continue;
        }
        const DWORD state = WaitForSingleObject(process, 0);
        CloseHandle(process);
        if (state == WAIT_OBJECT_0) return false;
    }
    return true;
}

ReleaseReport ReleaseOccupants(const std::vector<DWORD>& pids,
                                const std::vector<wchar_t>& letters) {
    ReleaseReport report;
    EnableDebugPrivilege();

    const DWORD self = GetCurrentProcessId();
    DWORD shell_pid = 0;
    if (HWND shell = GetShellWindow()) {
        GetWindowThreadProcessId(shell, &shell_pid);
    }

    for (DWORD pid : pids) {
        if (pid == 0 || pid == 4 || pid == self) continue;

        const std::wstring image = ProcessImageName(pid);
        const std::wstring base = text::ToLower(
            text::BaseNameWithoutExtension(image));

        // explorer 承载桌面和任务栏：只关闭相关窗口，绝不结束进程。
        if (base == L"explorer" || pid == shell_pid) {
            USB_LOG_INFO(L"occupants",
                         L"跳过结束：explorer/shell pid=%lu image=%s",
                         static_cast<unsigned long>(pid), image.c_str());
            ++report.explorer;
            continue;
        }
        if (!base.empty() && IsCriticalImage(base)) {
            USB_LOG_WARN(L"occupants", L"跳过结束：关键进程 pid=%lu image=%s",
                         static_cast<unsigned long>(pid), image.c_str());
            ++report.skipped;
            continue;
        }

        HANDLE process =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                            PROCESS_TERMINATE,
                        FALSE, pid);
        if (!process) {
            const DWORD error = GetLastError();
            USB_LOG_WARN(L"occupants",
                         L"结束进程失败：pid=%lu OpenProcess error=0x%08lX",
                         static_cast<unsigned long>(pid),
                         static_cast<unsigned long>(error));
            if (error == ERROR_ACCESS_DENIED) {
                ++report.denied;
            } else {
                ++report.skipped;
            }
            continue;
        }

        if (!TerminateProcess(process, 1)) {
            const DWORD error = GetLastError();
            USB_LOG_WARN(L"occupants",
                         L"结束进程失败：pid=%lu TerminateProcess error=0x%08lX",
                         static_cast<unsigned long>(pid),
                         static_cast<unsigned long>(error));
            if (error == ERROR_ACCESS_DENIED) {
                ++report.denied;
            } else {
                ++report.skipped;
            }
            CloseHandle(process);
            continue;
        }

        // TerminateProcess 已成功即视为“已解除”；这里最多再等 1.5s，让句柄
        // 有时间释放。进程退出慢不应被误判成“系统关键进程”而阻止重试。
        USB_LOG_INFO(L"occupants", L"已结束进程：pid=%lu image=%s",
                     static_cast<unsigned long>(pid), image.c_str());
        ++report.closed;
        WaitForSingleObject(process, 1500);
        CloseHandle(process);
    }

    report.explorer_windows = CloseExplorerWindows(letters);
    USB_LOG_INFO(
        L"occupants",
        L"释放完成：closed=%d explorer=%d denied=%d skipped=%d windows=%d",
        report.closed, report.explorer, report.denied, report.skipped,
        report.explorer_windows);
    return report;
}

}  // namespace usb
