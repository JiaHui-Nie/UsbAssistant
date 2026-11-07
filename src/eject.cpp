#include "eject.h"

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <string>
#include <vector>

// GUID_DEVINTERFACE_VOLUME / GUID_DEVINTERFACE_DISK，避免对 libuuid 的链接依赖
static const GUID kGuidDevInterfaceVolume = {
    0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
static const GUID kGuidDevInterfaceDisk = {
    0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

namespace {

std::wstring Lower(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(towlower(ch));
    return out;
}

// 从 "\\?\Volume{xxxx}\" 或 "\\?\STORAGE#Volume#{xxxx}#{...}" 中提取卷 GUID
std::wstring ExtractVolumeId(const std::wstring& path) {
    std::wstring low = Lower(path);
    size_t pos = low.find(L"volume{");
    if (pos == std::wstring::npos) return L"";
    size_t start = pos + 7;
    size_t end = low.find(L'}', start);
    if (end == std::wstring::npos) return L"";
    std::wstring id = low.substr(start, end - start);
    std::wstring out;
    out.reserve(id.size());
    for (wchar_t ch : id) {
        if (ch != L'-') out.push_back(ch);
    }
    return out;
}

std::wstring DeviceInstanceId(DEVINST devInst) {
    wchar_t buffer[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(devInst, buffer, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
        return L"";
    }
    return buffer;
}

bool IsRootLike(const std::wstring& instanceId) {
    std::wstring id = Lower(instanceId);
    return id.find(L"root_hub") != std::wstring::npos ||
           id.find(L"usb\\hub") != std::wstring::npos ||
           id.find(L"pci\\") != std::wstring::npos ||
           id.find(L"pnp0") != std::wstring::npos;
}

// 枚举 GUID 设备接口，用回调匹配目标
template <typename MatchFn>
DEVINST FindDevInst(const GUID& interfaceGuid, MatchFn match) {
    HDEVINFO set = SetupDiGetClassDevsW(&interfaceGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    DEVINST found = 0;
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &interfaceGuid, index, &interfaceData);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &required,
                                         nullptr);
        if (required == 0) continue;

        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA infoData = {};
        infoData.cbSize = sizeof(infoData);

        if (SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, required,
                                             &required, &infoData)) {
            if (match(detail->DevicePath)) {
                found = infoData.DevInst;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// 通过磁盘设备号找到物理磁盘的设备节点
DEVINST FindDiskDevInst(unsigned int diskNumber) {
    return FindDevInst(kGuidDevInterfaceDisk, [diskNumber](const wchar_t* path) {
        HANDLE handle = CreateFileW(path, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        STORAGE_DEVICE_NUMBER number = {};
        DWORD returned = 0;
        bool matched = DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr,
                                       0, &number, sizeof(number), &returned, nullptr) &&
                       number.DeviceType == FILE_DEVICE_DISK &&
                       number.DeviceNumber == diskNumber;
        CloseHandle(handle);
        return matched;
    });
}

// 通过卷 GUID 找到卷设备节点
DEVINST FindVolumeDevInst(const std::wstring& volumePath) {
    std::wstring target = ExtractVolumeId(volumePath);
    if (target.empty()) return 0;
    return FindDevInst(kGuidDevInterfaceVolume, [&target](const wchar_t* path) {
        return ExtractVolumeId(path) == target;
    });
}

std::wstring VetoMessage(PNP_VETO_TYPE type, const std::wstring& vetoName) {
    std::wstring base;
    switch (type) {
        case PNP_VetoLegacyDevice:
            base = L"设备正在被占用：有程序或驱动没有响应移除请求。";
            break;
        case PNP_VetoOutstandingOpen:
            base = L"设备正在被占用：仍有程序打开着该磁盘上的文件。";
            break;
        case PNP_VetoWindowsApp:
            base = L"设备正在被占用：有应用程序正在使用该设备。";
            break;
        case PNP_VetoWindowsService:
            base = L"设备正在被占用：有系统服务正在使用该设备。";
            break;
        case PNP_VetoDevice:
            base = L"设备拒绝了移除请求，请稍后重试。";
            break;
        case PNP_VetoDriver:
            base = L"驱动阻止了设备移除，请稍后重试。";
            break;
        case PNP_VetoInsufficientPower:
            base = L"电源状态不允许移除该设备。";
            break;
        case PNP_VetoNonDisableable:
            base = L"该设备不支持被停用或移除。";
            break;
        default:
            base = L"设备暂时无法移除，可能仍被占用。";
            break;
    }
    if (!vetoName.empty() && vetoName.size() < 80) {
        base += L"（" + vetoName + L"）";
    }
    return base;
}

std::wstring VolumeName(wchar_t letter) {
    if (letter == 0) return L"该分区";
    return std::wstring(1, letter) + L":";
}

struct PnpEjectResult {
    bool attempted = false;
    bool ok = false;
    bool vetoed = false;
    PNP_VETO_TYPE vetoType = PNP_VetoTypeUnknown;
    std::wstring vetoName;
    CONFIGRET lastError = CR_SUCCESS;
};

// 收集磁盘节点到 USB 设备节点的链路，并从最上层（最接近 USB 总线）开始请求移除。
// 直接在磁盘节点上请求移除通常会被存储驱动否决（PNP_VetoTypeUnknown），
// 必须请求移除它的父设备（USB\VID_...），整台设备才会断开。
PnpEjectResult TryPnpEject(unsigned int diskNumber,
                           const std::wstring& volumePathHint) {
    PnpEjectResult result;

    DEVINST node = FindDiskDevInst(diskNumber);
    if (node == 0) {
        DEVINST volumeNode = FindVolumeDevInst(volumePathHint);
        if (volumeNode != 0) {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, volumeNode, 0) == CR_SUCCESS) node = parent;
        }
    }
    if (node == 0) return result;

    // 记录从磁盘到 USB 设备（不含 hub / PCI 根）的节点链
    DEVINST chain[4] = {};
    int count = 0;
    while (node != 0 && count < 4) {
        std::wstring id = DeviceInstanceId(node);
        if (id.empty() || IsRootLike(id)) break;
        chain[count++] = node;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    if (count == 0) return result;

    for (int index = count - 1; index >= 0; --index) {
        wchar_t vetoText[256] = {};
        PNP_VETO_TYPE vetoType = PNP_VetoTypeUnknown;
        result.attempted = true;
        CONFIGRET ret =
            CM_Request_Device_EjectW(chain[index], &vetoType, vetoText, 255, 0);
        if (ret == CR_SUCCESS) {
            result.ok = true;
            return result;
        }
        if (ret == CR_REMOVE_VETOED) {
            if (!result.vetoed) { // 只保留最上层（最贴近真实原因）的否决信息
                result.vetoed = true;
                result.vetoType = vetoType;
                result.vetoName = vetoText;
            }
            continue;
        }
        if (ret == CR_NO_SUCH_DEVNODE || ret == CR_INVALID_DEVNODE) continue;
        result.lastError = ret;
    }
    return result;
}

// 直接向卷发送弹出介质指令（读卡器 / 部分 U 盘可用）
// 卷 GUID 路径末尾的反斜杠会让 CreateFile 返回 ERROR_PATH_NOT_FOUND
std::wstring OpenableVolumePath(const std::wstring& volumePath) {
    std::wstring path = volumePath;
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}

bool MediaEject(const std::wstring& volumePath, DWORD* error) {
    *error = 0;
    HANDLE handle = CreateFileW(OpenableVolumePath(volumePath).c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }

    DWORD returned = 0;
    // 被占用时 FSCTL_LOCK_VOLUME 会立即失败，无需长重试；
    // 少量重试只用于等待刚释放的句柄
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                            &returned, nullptr)) {
            break;
        }
        Sleep(60);
    }
    DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned,
                    nullptr);

    bool ok = DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr,
                              0, &returned, nullptr) != FALSE;
    if (!ok) *error = GetLastError();

    DeviceIoControl(handle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &returned,
                    nullptr);
    CloseHandle(handle);
    return ok;
}

// 卷盘符是否仍存在
bool AnyVolumeLetterPresent(const std::vector<EjectVolumeTarget>& volumes) {
    DWORD drives = GetLogicalDrives();
    for (const auto& volume : volumes) {
        if (volume.letter == 0) continue;
        int index = towlower(volume.letter) - L'a';
        if (index >= 0 && index < 26 && (drives & (1u << index)) != 0) return true;
    }
    return false;
}

// 部分设备（尤其是被识别成固定磁盘的移动硬盘）对弹出指令返回成功，
// 但并没有真正断开。以磁盘设备节点是否消失为准；磁盘节点偶尔会残留，
// 此时若所有分区盘符都已消失，也按已弹出处理。
bool WaitDiskGone(unsigned int diskNumber,
                  const std::vector<EjectVolumeTarget>& volumes, DWORD timeoutMs) {
    bool hasLetter = false;
    for (const auto& volume : volumes) {
        if (volume.letter != 0) hasLetter = true;
    }
    DWORD waited = 0;
    while (true) {
        if (FindDiskDevInst(diskNumber) == 0) return true;
        if (hasLetter && !AnyVolumeLetterPresent(volumes)) return true;
        if (waited >= timeoutMs) return false;
        Sleep(150);
        waited += 150;
    }
}

} // namespace

EjectOutcome EjectDisk(unsigned int diskNumber, const std::wstring& title,
                       const std::vector<EjectVolumeTarget>& volumes) {
    EjectOutcome outcome;
    std::wstring deviceName = title.empty() ? L"该设备" : L"「" + title + L"」";

    if (volumes.empty()) {
        outcome.status = EjectStatus::NotFound;
        outcome.message = deviceName + L" 没有可弹出的分区，可能已被拔出。";
        return outcome;
    }

    // 1. 逐个锁定并卸载所有分区
    struct OpenedVolume {
        HANDLE handle = INVALID_HANDLE_VALUE;
        EjectVolumeTarget target;
    };
    std::vector<OpenedVolume> opened;
    opened.reserve(volumes.size());

    auto closeAll = [&opened](bool unlock) {
        for (auto& item : opened) {
            if (item.handle == INVALID_HANDLE_VALUE) continue;
            DWORD returned = 0;
            if (unlock) {
                DeviceIoControl(item.handle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr,
                                0, &returned, nullptr);
            }
            CloseHandle(item.handle);
            item.handle = INVALID_HANDLE_VALUE;
        }
    };

    for (const auto& target : volumes) {
        HANDLE handle = CreateFileW(OpenableVolumePath(target.path).c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            closeAll(false);
            if (error == ERROR_ACCESS_DENIED) {
                outcome.status = EjectStatus::AccessDenied;
                outcome.message = L"权限不足：无法锁定 " + VolumeName(target.letter) +
                                  L" 分区。请以管理员身份运行后重试。";
            } else if (error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) {
                outcome.status = EjectStatus::NotFound;
                outcome.message = deviceName + L" 尚未就绪，可能已被拔出。";
            } else {
                outcome.status = EjectStatus::NotFound;
                outcome.message = L"无法打开 " + VolumeName(target.letter) +
                                  L" 分区，可能已被拔出或正在被独占。";
            }
            return outcome;
        }

        bool locked = false;
        DWORD returned = 0;
        // 被占用时锁定会立即失败，重试仅用于等待刚释放的句柄，避免拖慢出错反馈
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &returned, nullptr)) {
                locked = true;
                break;
            }
            Sleep(60);
        }
        if (!locked) {
            DWORD error = GetLastError();
            CloseHandle(handle);
            closeAll(true);
            if (error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION ||
                error == ERROR_SHARING_VIOLATION) {
                outcome.status = EjectStatus::Busy;
                outcome.message = L"设备正在被占用：" + VolumeName(target.letter) +
                                  L" 分区仍被其他程序读写。请关闭相关程序或文件后重试。";
            } else {
                outcome.status = EjectStatus::Failed;
                outcome.message = L"无法锁定 " + VolumeName(target.letter) +
                                  L" 分区（错误码 " + std::to_wstring(error) + L"）。";
            }
            return outcome;
        }

        // 卸载卷并阻止后续介质访问
        DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                        &returned, nullptr);
        PREVENT_MEDIA_REMOVAL prevent = {};
        prevent.PreventMediaRemoval = TRUE;
        DeviceIoControl(handle, IOCTL_STORAGE_MEDIA_REMOVAL, &prevent, sizeof(prevent),
                        nullptr, 0, &returned, nullptr);

        OpenedVolume item;
        item.handle = handle;
        item.target = target;
        opened.push_back(item);
    }

    // 2. 请求 PnP 移除；若被自身打开的卷句柄否决，释放句柄后重试一次
    PnpEjectResult pnp = TryPnpEject(diskNumber, volumes.front().path);
    if (pnp.ok) {
        closeAll(false);
        if (WaitDiskGone(diskNumber, volumes, 4000)) {
            outcome.status = EjectStatus::Ok;
            outcome.message = L"已安全弹出" + (title.empty() ? L"设备" : deviceName) +
                              L"，现在可以拔下设备了。";
            return outcome;
        }
        outcome.status = EjectStatus::Failed;
        outcome.message = deviceName + L" 未能真正移除：系统仍报告该磁盘存在。";
        return outcome;
    }

    closeAll(false);

    PnpEjectResult retry;
    if (pnp.vetoed || pnp.attempted) {
        retry = TryPnpEject(diskNumber, volumes.front().path);
        if (retry.ok) {
            if (WaitDiskGone(diskNumber, volumes, 4000)) {
                outcome.status = EjectStatus::Ok;
                outcome.message = L"已安全弹出" +
                                  (title.empty() ? L"设备" : deviceName) +
                                  L"，现在可以拔下设备了。";
                return outcome;
            }
            outcome.status = EjectStatus::Failed;
            outcome.message = deviceName + L" 未能真正移除：系统仍报告该磁盘存在。";
            return outcome;
        }
    }

    // 明确是「有程序打开着该磁盘」的否决：直接按占用返回，不再走耗时的
    // 介质弹出与等待，保证占用界面能立刻弹出
    if (retry.vetoed && (retry.vetoType == PNP_VetoOutstandingOpen ||
                         retry.vetoType == PNP_VetoWindowsApp)) {
        outcome.status = EjectStatus::Busy;
        outcome.message = VetoMessage(retry.vetoType, retry.vetoName);
        return outcome;
    }

    // 3. 回退：直接弹出介质（读卡器、部分 U 盘）
    DWORD mediaError = 0;
    if (MediaEject(volumes.front().path, &mediaError)) {
        if (WaitDiskGone(diskNumber, volumes, 3500)) {
            outcome.status = EjectStatus::Ok;
            outcome.message = L"已安全弹出" + (title.empty() ? L"设备" : deviceName) +
                              L"，现在可以拔下设备了。";
            return outcome;
        }
        outcome.status = EjectStatus::Failed;
        outcome.message = deviceName +
                          L" 未能真正断开：介质已卸载但设备仍在。Windows 可能把该"
                          L"移动硬盘识别为固定磁盘，可在设备属性中启用「快速删除」后重试。";
        return outcome;
    }

    // PnP 明确给出否决原因，且介质弹出也不被支持时，否决信息比错误码更有参考价值
    if (retry.vetoed &&
        (mediaError == 0 || mediaError == ERROR_INVALID_FUNCTION ||
         mediaError == ERROR_NOT_SUPPORTED)) {
        outcome.status = EjectStatus::Busy;
        outcome.message = VetoMessage(retry.vetoType, retry.vetoName);
        return outcome;
    }

    if (mediaError == ERROR_ACCESS_DENIED) {
        outcome.status = EjectStatus::AccessDenied;
        outcome.message = L"权限不足，设备拒绝移除。请以管理员身份运行后重试。";
    } else if (mediaError == ERROR_INVALID_FUNCTION ||
               mediaError == ERROR_NOT_SUPPORTED) {
        outcome.status = EjectStatus::Unsupported;
        outcome.message = L"该设备不支持软件弹出，确认没有程序读写后可直接拔出。";
    } else if (mediaError == ERROR_NOT_READY || mediaError == ERROR_NO_MEDIA_IN_DRIVE) {
        outcome.status = EjectStatus::NotFound;
        outcome.message = deviceName + L" 已不在位，可能已被拔出。";
    } else {
        DWORD code = mediaError != 0 ? mediaError : static_cast<DWORD>(pnp.lastError);
        outcome.status = EjectStatus::Failed;
        outcome.message = L"弹出失败（错误码 " + std::to_wstring(code) +
                          L"）。请关闭占用程序后重试，或直接拔出设备。";
    }
    return outcome;
}
