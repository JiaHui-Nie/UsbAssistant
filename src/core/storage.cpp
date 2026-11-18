#include "core/storage.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "core/text.h"

namespace usb::storage {
namespace {

// GUID_DEVINTERFACE_DISK。手写 GUID 避免依赖 libuuid。
const GUID kDiskInterface = {
    0x53f56307, 0xb6bf, 0x11d0,
    {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

const GUID kEspGuid = {0xc12a7328, 0xf81f, 0x11d2,
                       {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};
const GUID kMsrGuid = {0xe3c9e316, 0x0b5c, 0x4db8,
                       {0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae}};

bool SameGuid(const GUID& left, const GUID& right) {
    return left.Data1 == right.Data1 && left.Data2 == right.Data2 &&
           left.Data3 == right.Data3 &&
           std::memcmp(left.Data4, right.Data4, sizeof(left.Data4)) == 0;
}

std::wstring ErrorMessage(DWORD error) {
    return L"（错误码 " + std::to_wstring(error) + L"）";
}

wchar_t PickFreeDriveLetter() {
    const DWORD drives = GetLogicalDrives();
    for (int code = 'Z'; code >= 'D'; --code) {
        const int index = code - 'A';
        if ((drives & (1u << index)) != 0) continue;
        wchar_t root[4] = {static_cast<wchar_t>(code), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR) {
            return static_cast<wchar_t>(code);
        }
    }
    return 0;
}

}  // namespace

std::wstring OpenableVolumePath(std::wstring volume_path) {
    while (!volume_path.empty() && volume_path.back() == L'\\') {
        volume_path.pop_back();
    }
    return volume_path;
}

DEVINST FindDiskDevInst(std::uint32_t disk_number) {
    HDEVINFO set = SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    DEVINST found = 0;
    SP_DEVICE_INTERFACE_DATA interface_data = {};
    interface_data.cbSize = sizeof(interface_data);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &kDiskInterface, index,
                                     &interface_data);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0,
                                         &required, nullptr);
        if (required == 0) continue;

        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
            buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info = {};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail,
                                              required, &required, &info)) {
            continue;
        }

        HANDLE device = CreateFileW(detail->DevicePath, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;

        STORAGE_DEVICE_NUMBER number = {};
        DWORD returned = 0;
        const bool matched =
            DeviceIoControl(device, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                            &number, sizeof(number), &returned, nullptr) &&
            number.DeviceType == FILE_DEVICE_DISK &&
            number.DeviceNumber == disk_number;
        CloseHandle(device);
        if (matched) {
            found = info.DevInst;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool IsDiskPresent(std::uint32_t disk_number) {
    return FindDiskDevInst(disk_number) != 0;
}

bool AnyTargetLetterPresent(const std::vector<EjectTarget>& targets) {
    const DWORD drives = GetLogicalDrives();
    for (const auto& target : targets) {
        if (target.letter == 0) continue;
        const int index = towlower(target.letter) - L'a';
        if (index >= 0 && index < 26 && (drives & (1u << index)) != 0) {
            return true;
        }
    }
    return false;
}

bool WaitDiskGone(std::uint32_t disk_number,
                  const std::vector<EjectTarget>& targets, int timeout_ms) {
    bool has_letter = false;
    for (const auto& target : targets) {
        if (target.letter != 0) has_letter = true;
    }

    int waited = 0;
    for (;;) {
        if (!IsDiskPresent(disk_number)) return true;
        if (has_letter && !AnyTargetLetterPresent(targets)) return true;
        if (waited >= timeout_ms) return false;
        Sleep(150);
        waited += 150;
    }
}

std::vector<PartitionEntry> ReadPartitionTable(std::uint32_t disk_number) {
    std::vector<PartitionEntry> entries;

    wchar_t path[64] = {};
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%u", disk_number);
    HANDLE disk = CreateFileW(path, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (disk == INVALID_HANDLE_VALUE) return entries;

    std::vector<BYTE> buffer(64 * 1024);
    DWORD returned = 0;
    BOOL ok = FALSE;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ok = DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                             buffer.data(),
                             static_cast<DWORD>(buffer.size()), &returned,
                             nullptr);
        if (ok) break;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        buffer.resize(returned > 0 ? returned + 4096 : buffer.size() * 2);
    }

    if (ok) {
        auto* layout =
            reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
        for (DWORD i = 0; i < layout->PartitionCount; ++i) {
            const PARTITION_INFORMATION_EX& entry = layout->PartitionEntry[i];
            const auto length =
                static_cast<std::uint64_t>(entry.PartitionLength.QuadPart);
            // 长度为 0 是空表项，PartitionNumber 为 0 是扩展分区容器。
            if (length == 0 || entry.PartitionNumber == 0) continue;

            PartitionEntry out;
            out.start = static_cast<std::uint64_t>(
                entry.StartingOffset.QuadPart);
            out.length = length;
            if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
                if (SameGuid(entry.Gpt.PartitionType, kEspGuid)) {
                    out.kind = PartitionKind::Esp;
                } else if (SameGuid(entry.Gpt.PartitionType, kMsrGuid)) {
                    out.kind = PartitionKind::Msr;
                }
            } else if (layout->PartitionStyle == PARTITION_STYLE_MBR &&
                       entry.Mbr.PartitionType == 0xEF) {
                out.kind = PartitionKind::Esp;
            }
            entries.push_back(out);
        }
    }

    CloseHandle(disk);
    return entries;
}

std::wstring DeviceInstanceId(DEVINST node) {
    wchar_t buffer[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(node, buffer, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
        return L"";
    }
    return buffer;
}

bool IsBusRoot(const std::wstring& instance_id) {
    const std::wstring id = text::ToLower(instance_id);
    return id.find(L"root_hub") != std::wstring::npos ||
           id.find(L"usb\\hub") != std::wstring::npos ||
           id.find(L"pci\\") != std::wstring::npos ||
           id.find(L"pnp0") != std::wstring::npos;
}

std::wstring PnpDisplayName(std::uint32_t disk_number) {
    DEVINST node = FindDiskDevInst(disk_number);
    if (node == 0) return L"";

    // 从磁盘节点向上收集最多 4 级，排除 USB Hub / PCI 根节点。
    DEVINST chain[4] = {};
    int count = 0;
    while (node != 0 && count < 4) {
        const std::wstring id = DeviceInstanceId(node);
        if (id.empty() || IsBusRoot(id)) break;
        chain[count++] = node;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }

    for (int i = 0; i < count; ++i) {
        wchar_t buffer[512] = {};
        ULONG size = sizeof(buffer);
        if (CM_Get_DevNode_Registry_PropertyW(chain[i],
                                              CM_DRP_FRIENDLYNAME, nullptr,
                                              buffer, &size, 0) != CR_SUCCESS) {
            continue;
        }
        const std::wstring name = text::CleanPnpName(buffer);
        if (!name.empty()) return name;
    }
    return L"";
}

std::vector<std::wstring> DosDevicePaths(const std::vector<wchar_t>& letters) {
    std::vector<std::wstring> paths;
    for (wchar_t letter : letters) {
        if (letter == 0) continue;
        wchar_t dos_name[3] = {letter, L':', 0};
        wchar_t target[1024] = {};
        if (!QueryDosDeviceW(dos_name, target, 1024)) continue;
        std::wstring path = text::ToLower(target);
        if (path.empty()) continue;
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::wstring FirstDriveLetter(const std::wstring& volume_name) {
    wchar_t mounts[512] = {};
    if (!GetVolumePathNamesForVolumeNameW(volume_name.c_str(), mounts, 512,
                                          nullptr)) {
        return L"";
    }
    for (const wchar_t* mount = mounts; *mount != L'\0';
         mount += wcslen(mount) + 1) {
        if (wcslen(mount) == 3 && mount[0] >= L'A' && mount[1] == L':' &&
            mount[2] == L'\\') {
            return mount;
        }
    }
    return L"";
}

MountResult AssignDriveLetter(const std::wstring& volume_path) {
    MountResult result;
    if (volume_path.empty()) {
        result.status = MountStatus::NotFound;
        result.message = L"找不到该分区卷，可能已被移除。";
        return result;
    }

    const wchar_t letter = PickFreeDriveLetter();
    if (letter == 0) {
        result.status = MountStatus::NoFreeLetter;
        result.message = L"没有可用的盘符，请先释放一个驱动器号。";
        return result;
    }

    std::wstring volume = volume_path;
    if (volume.back() != L'\\') volume.push_back(L'\\');
    wchar_t root[4] = {letter, L':', L'\\', 0};
    if (!SetVolumeMountPointW(root, volume.c_str())) {
        const DWORD error = GetLastError();
        switch (error) {
            case ERROR_ACCESS_DENIED:
                result.status = MountStatus::AccessDenied;
                result.message =
                    L"分配盘符需要管理员权限，请以管理员身份运行 U 盘助手"
                    L"后重试。";
                break;
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                result.status = MountStatus::NotFound;
                result.message = L"分区卷已不存在，请重新插拔设备。";
                break;
            case ERROR_INVALID_PARAMETER:
            case ERROR_NOT_SUPPORTED:
                result.status = MountStatus::Unsupported;
                result.message = L"该分区不支持分配盘符。";
                break;
            default:
                result.status = MountStatus::Failed;
                result.message = L"分配盘符失败" + ErrorMessage(error);
                break;
        }
        return result;
    }

    result.status = MountStatus::Ok;
    result.letter = letter;
    return result;
}

wchar_t FindVolumeDriveLetter(const std::wstring& volume_path) {
    if (volume_path.empty()) return 0;
    std::wstring volume = volume_path;
    if (volume.back() != L'\\') volume.push_back(L'\\');

    wchar_t paths[1024] = {};
    DWORD length = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume.c_str(), paths,
                                           static_cast<DWORD>(std::size(paths)),
                                           &length)) {
        return 0;
    }
    for (const wchar_t* current = paths; *current != L'\0';
         current += wcslen(current) + 1) {
        if (current[0] != L'\0' && current[1] == L':') return current[0];
    }
    return 0;
}

DWORD ProbeDirectoryAccess(const std::wstring& path) {
    if (path.empty()) return ERROR_INVALID_PARAMETER;
    HANDLE handle =
        CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(handle);
    return ERROR_SUCCESS;
}

MountResult RemoveDriveLetter(wchar_t letter) {    MountResult result;
    if (letter == 0) {
        result.status = MountStatus::NotFound;
        result.message = L"该分区没有盘符。";
        return result;
    }

    wchar_t root[4] = {letter, L':', L'\\', 0};
    if (!DeleteVolumeMountPointW(root)) {
        const DWORD error = GetLastError();
        switch (error) {
            case ERROR_ACCESS_DENIED:
                result.status = MountStatus::AccessDenied;
                result.message =
                    L"取消挂载需要管理员权限，请以管理员身份运行 U 盘助手"
                    L"后重试。";
                break;
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                result.status = MountStatus::NotFound;
                result.message = L"该盘符已不存在，可能已被手动移除。";
                break;
            default:
                result.status = MountStatus::Failed;
                result.message = L"取消挂载失败" + ErrorMessage(error);
                break;
        }
        return result;
    }

    result.status = MountStatus::Ok;
    result.letter = letter;
    return result;
}

std::wstring VolumeDisplayName(wchar_t letter) {
    if (letter == 0) return L"该分区";
    return std::wstring(1, letter) + L":";
}

}  // namespace usb::storage
