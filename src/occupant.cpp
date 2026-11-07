#include "occupant.h"

#include <shellapi.h>
#include <winver.h>
#include <winternl.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

typedef LONG(NTAPI* NtQuerySystemInformationFn)(ULONG, void*, ULONG, ULONG*);
typedef LONG(NTAPI* NtQueryObjectFn)(HANDLE, ULONG, void*, ULONG, ULONG*);

const ULONG kSystemExtendedHandleInformation = 64;
const ULONG kObjectNameInformation = 1;
const ULONG kObjectTypeInformation = 2;
const LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004u);

NtQuerySystemInformationFn NtQuerySystemInformationPtr = nullptr;
NtQueryObjectFn NtQueryObjectPtr = nullptr;

void ResolveNtoskrnl() {
    if (NtQuerySystemInformationPtr && NtQueryObjectPtr) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    NtQuerySystemInformationPtr = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    NtQueryObjectPtr = reinterpret_cast<NtQueryObjectFn>(
        GetProcAddress(ntdll, "NtQueryObject"));
}

struct HandleEntryEx {
    void* object;
    ULONG_PTR pid;
    ULONG_PTR handleValue;
    ULONG grantedAccess;
    USHORT creatorBackTraceIndex;
    USHORT objectTypeIndex;
    ULONG handleAttributes;
    ULONG reserved;
};

struct HandleInfoEx {
    ULONG_PTR count;
    ULONG_PTR reserved;
    HandleEntryEx entries[1];
};

struct ObjectTypeInfo {
    UNICODE_STRING typeName;
};

std::wstring Lower(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(towlower(ch));
    return out;
}

// 盘符 -> NT 设备路径（如 "\Device\HarddiskVolume3"）
std::vector<std::wstring> DevicePaths(const std::vector<wchar_t>& letters) {
    std::vector<std::wstring> paths;
    for (wchar_t letter : letters) {
        if (letter == 0) continue;
        wchar_t dosName[3] = {letter, L':', 0};
        wchar_t target[1024] = {};
        if (!QueryDosDeviceW(dosName, target, 1024)) continue;
        std::wstring path = Lower(target);
        if (path.empty()) continue;
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    return paths;
}

// 句柄对象名是否落在这些设备路径下
bool MatchesDevice(const std::wstring& lowerName,
                   const std::vector<std::wstring>& devices) {
    for (const std::wstring& device : devices) {
        if (lowerName.size() <= device.size()) {
            if (lowerName == device) return true;
            continue;
        }
        if (lowerName.compare(0, device.size(), device) == 0 &&
            lowerName[device.size()] == L'\\') {
            return true;
        }
    }
    return false;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translation = nullptr;
    UINT translationSize = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translation), &translationSize) ||
        translation == nullptr || translationSize < sizeof(Translation)) {
        return L"";
    }

    wchar_t tag[16] = {};
    wsprintfW(tag, L"%04x%04x", translation->language, translation->codePage);
    std::wstring key = L"\\StringFileInfo\\";
    key += tag;
    key += L"\\FileDescription";
    wchar_t* value = nullptr;
    UINT valueSize = 0;
    if (VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value),
                       &valueSize) &&
        value && valueSize > 1) {
        return std::wstring(value);
    }
    return L"";
}

std::wstring BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

HICON ExtractIcon(const std::wstring& path) {
    SHFILEINFOW info = {};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_LARGEICON) &&
        info.hIcon) {
        return info.hIcon;
    }
    return nullptr;
}

// 打开进程句柄并缓存，供逐个句柄复制时复用
HANDLE ProcessHandle(DWORD pid, std::map<DWORD, HANDLE>& cache) {
    auto it = cache.find(pid);
    if (it != cache.end()) return it->second;
    HANDLE handle = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, pid);
    cache[pid] = handle;
    return handle;
}

// 句柄对象类型是否为 File
bool QueryFileType(HANDLE handle) {
    BYTE typeBuffer[512] = {};
    ULONG typeLength = 0;
    LONG status = NtQueryObjectPtr(handle, kObjectTypeInformation, typeBuffer,
                                   sizeof(typeBuffer), &typeLength);
    if (status != 0) return false;
    auto* typeInfo = reinterpret_cast<ObjectTypeInfo*>(typeBuffer);
    return typeInfo->typeName.Buffer && typeInfo->typeName.Length >= 8 &&
           _wcsicmp(typeInfo->typeName.Buffer, L"File") == 0;
}

} // namespace

std::vector<VolumeOccupant> EnumerateVolumeOccupants(
    const std::vector<wchar_t>& letters) {
    ResolveNtoskrnl();
    if (!NtQuerySystemInformationPtr || !NtQueryObjectPtr) return {};

    std::vector<std::wstring> devices = DevicePaths(letters);
    if (devices.empty()) return {};

    // 1. 枚举全系统句柄（缓冲区不足时按返回长度重试）
    ULONG size = 1 << 20;
    std::vector<BYTE> buffer;
    LONG status = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        buffer.resize(size);
        ULONG returned = 0;
        status = NtQuerySystemInformationPtr(kSystemExtendedHandleInformation,
                                             buffer.data(), size, &returned);
        if (status == kStatusInfoLengthMismatch) {
            size = returned + 65536;
            continue;
        }
        size = returned;
        break;
    }
    if (status != 0 || size == 0) return {};

    auto* info = reinterpret_cast<HandleInfoEx*>(buffer.data());
    DWORD self = GetCurrentProcessId();

    std::map<DWORD, HANDLE> processCache;

    // 2. 预解析对象类型索引：每种类型只查询一次，之后按索引快速排除非文件句柄
    //    （系统句柄动辄数万，逐个查询类型会让界面长时间停在“正在查找”）
    std::map<USHORT, int> typeState; // 1 = File, 2 = 非 File
    std::map<USHORT, int> typeTries;
    for (size_t i = 0; i < info->count; ++i) {
        const HandleEntryEx& entry = info->entries[i];
        USHORT typeIndex = entry.objectTypeIndex;
        if (typeState.count(typeIndex)) continue;
        if (typeTries[typeIndex] >= 6) continue;
        typeTries[typeIndex]++;
        DWORD pid = static_cast<DWORD>(entry.pid);
        if (pid == 0 || pid == 4 || pid == self) continue;

        HANDLE source = ProcessHandle(pid, processCache);
        if (!source) continue;
        HANDLE dup = nullptr;
        if (!DuplicateHandle(source, reinterpret_cast<HANDLE>(entry.handleValue),
                             GetCurrentProcess(), &dup, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }
        typeState[typeIndex] = QueryFileType(dup) ? 1 : 2;
        CloseHandle(dup);
    }

    // 3. 逐个复制文件句柄，比对对象名是否落在目标设备上
    std::set<DWORD> matched;
    ULONG64 started = GetTickCount64();
    for (size_t i = 0; i < info->count; ++i) {
        if (GetTickCount64() - started > 15000) break; // 兜底，避免长时间无结果
        const HandleEntryEx& entry = info->entries[i];
        DWORD pid = static_cast<DWORD>(entry.pid);
        if (pid == 0 || pid == 4 || pid == self) continue;
        if (matched.count(pid)) continue;

        auto stateIt = typeState.find(entry.objectTypeIndex);
        if (stateIt != typeState.end() && stateIt->second == 2) continue;

        HANDLE source = ProcessHandle(pid, processCache);
        if (!source) continue;

        HANDLE dup = nullptr;
        if (!DuplicateHandle(source, reinterpret_cast<HANDLE>(entry.handleValue),
                             GetCurrentProcess(), &dup, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }

        // 管道等句柄查询对象名可能永久阻塞，先用文件类型排除
        if (GetFileType(dup) != FILE_TYPE_DISK) {
            CloseHandle(dup);
            continue;
        }
        if (stateIt == typeState.end() || stateIt->second != 1) {
            if (!QueryFileType(dup)) {
                CloseHandle(dup);
                continue;
            }
            typeState[entry.objectTypeIndex] = 1;
        }

        wchar_t nameBuffer[4096] = {};
        ULONG nameLength = 0;
        if (NtQueryObjectPtr(dup, kObjectNameInformation, nameBuffer,
                             sizeof(nameBuffer) - 1, &nameLength) == 0 &&
            nameLength > 0) {
            auto* nameInfo = reinterpret_cast<UNICODE_STRING*>(nameBuffer);
            if (nameInfo->Buffer && nameInfo->Length > 0) {
                std::wstring name = Lower(
                    std::wstring(nameInfo->Buffer, nameInfo->Length / sizeof(wchar_t)));
                if (MatchesDevice(name, devices)) matched.insert(pid);
            }
        }
        CloseHandle(dup);
    }
    for (auto& item : processCache) {
        if (item.second) CloseHandle(item.second);
    }

    // 4. 取进程名和图标
    std::vector<VolumeOccupant> list;
    for (DWORD pid : matched) {
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!handle) continue;
        wchar_t path[MAX_PATH] = {};
        DWORD pathSize = MAX_PATH;
        bool ok = QueryFullProcessImageNameW(handle, 0, path, &pathSize) != FALSE;
        CloseHandle(handle);
        if (!ok || path[0] == 0) continue;

        VolumeOccupant occupant;
        occupant.pid = pid;
        occupant.path = path;
        occupant.name = FileDescription(occupant.path);
        if (occupant.name.empty()) occupant.name = BaseName(occupant.path);
        occupant.icon = ExtractIcon(occupant.path);
        list.push_back(std::move(occupant));
    }

    std::sort(list.begin(), list.end(),
              [](const VolumeOccupant& a, const VolumeOccupant& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    return list;
}

int TerminateOccupants(const std::vector<DWORD>& pids) {
    DWORD self = GetCurrentProcessId();
    // explorer 承载桌面与任务栏，结束它会导致 Windows 桌面消失，必须跳过
    DWORD shellPid = 0;
    HWND shell = GetShellWindow();
    if (shell) GetWindowThreadProcessId(shell, &shellPid);

    int count = 0;
    for (DWORD pid : pids) {
        if (pid == 0 || pid == 4 || pid == self || pid == shellPid) continue;

        // 再按进程映像名兜底排除 explorer.exe
        bool isExplorer = false;
        HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (query) {
            wchar_t path[MAX_PATH] = {};
            DWORD pathSize = MAX_PATH;
            if (QueryFullProcessImageNameW(query, 0, path, &pathSize)) {
                isExplorer = Lower(BaseName(std::wstring(path))) == L"explorer";
            }
            CloseHandle(query);
        }
        if (isExplorer) continue;

        HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!handle) continue;
        if (TerminateProcess(handle, 1)) ++count;
        CloseHandle(handle);
    }
    return count;
}

void FreeOccupants(std::vector<VolumeOccupant>* list) {
    if (!list) return;
    for (auto& item : *list) {
        if (item.icon) DestroyIcon(item.icon);
        item.icon = nullptr;
    }
    list->clear();
}
