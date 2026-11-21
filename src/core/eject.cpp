#include "core/eject.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/storage.h"
#include "core/text.h"

namespace usb {
namespace {

struct PnpOutcome {
    bool attempted = false;
    bool node_found = false;
    bool ok = false;
    bool vetoed = false;
    PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
    std::wstring veto_name;
    CONFIGRET last_error = CR_SUCCESS;
};

struct MediaOutcome {
    bool ok = false;
    bool busy = false;
    DWORD error = 0;
};

struct ForceOutcome {
    bool ok = false;
    bool denied = false;
    bool busy = false;
    bool pending_removal = false;
    DWORD error = 0;
};

const GUID kVolumeInterface = {
    0x53f5630d, 0xb6bf, 0x11d0,
    {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

std::wstring ExtractVolumeId(const std::wstring& path) {
    const std::wstring lower = text::ToLower(path);
    const std::size_t pos = lower.find(L"volume{");
    if (pos == std::wstring::npos) return L"";
    const std::size_t start = pos + 7;
    const std::size_t end = lower.find(L'}', start);
    if (end == std::wstring::npos) return L"";

    std::wstring id;
    for (std::size_t i = start; i < end; ++i) {
        if (lower[i] != L'-') id.push_back(lower[i]);
    }
    return id;
}

template <typename Match>
DEVINST FindDevInst(const GUID& interface_guid, Match match) {
    HDEVINFO set = SetupDiGetClassDevsW(&interface_guid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    DEVINST found = 0;
    SP_DEVICE_INTERFACE_DATA interface_data = {};
    interface_data.cbSize = sizeof(interface_data);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &interface_guid, index,
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
        if (SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail,
                                              required, &required, &info) &&
            match(detail->DevicePath)) {
            found = info.DevInst;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}

DEVINST FindVolumeDevInst(const std::wstring& volume_path) {
    const std::wstring target = ExtractVolumeId(volume_path);
    if (target.empty()) return 0;
    return FindDevInst(kVolumeInterface, [&target](const wchar_t* path) {
        return ExtractVolumeId(path) == target;
    });
}

bool IsOccupancyVeto(PNP_VETO_TYPE type) {
    switch (type) {
        case PNP_VetoOutstandingOpen:
        case PNP_VetoWindowsApp:
        case PNP_VetoWindowsService:
        case PNP_VetoLegacyDevice:
            return true;
        default:
            return false;
    }
}

std::wstring VetoMessage(PNP_VETO_TYPE type, const std::wstring& name) {
    std::wstring message;
    switch (type) {
        case PNP_VetoLegacyDevice:
            message = L"有程序或驱动没有响应移除请求。";
            break;
        case PNP_VetoOutstandingOpen:
            message = L"仍有程序打开着该磁盘上的文件。";
            break;
        case PNP_VetoWindowsApp:
            message = L"有应用程序正在使用该设备。";
            break;
        case PNP_VetoWindowsService:
            message = L"有系统服务正在使用该设备。";
            break;
        case PNP_VetoDevice:
            message = L"设备拒绝了移除请求。";
            break;
        case PNP_VetoDriver:
            message = L"驱动阻止了设备移除。";
            break;
        case PNP_VetoInsufficientPower:
            message = L"电源状态不允许移除该设备。";
            break;
        case PNP_VetoNonDisableable:
            message = L"该设备不支持被停用或移除。";
            break;
        default:
            message = L"设备暂时无法移除，可能仍被占用。";
            break;
    }
    if (!name.empty() && name.size() < 80) {
        message += L"（" + name + L"）";
    }
    return message;
}

// USB Hub（含主板前置面板、扩展坞里的内置 Hub）不是可以物理拔出的终端
// 设备。对它调用 CM_Request_Device_EjectW 会把它留在「已准备安全删除但
// 仍在」的状态（设备管理器代码 47），该 Hub 下的端口随后不再枚举新设备。
bool IsUsbHubNode(DEVINST node) {
    wchar_t service[64] = {};
    ULONG size = sizeof(service);
    if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_SERVICE, nullptr,
                                          service, &size, 0) != CR_SUCCESS) {
        return false;
    }
    const std::wstring name = text::ToLower(service);
    return name == L"usbhub" || name == L"usbhub2" || name == L"usbhub3";
}

bool IsUsbDeviceNode(const std::wstring& instance_id) {
    const std::wstring id = text::ToLower(instance_id);
    return id.rfind(L"usb\\", 0) == 0 &&
           id.find(L"root_hub") == std::wstring::npos;
}

// 从磁盘节点向 USB 总线上溯，只对最上层的 USB 设备节点请求移除。
// Hub 与总线根都不是可物理拔出的设备，绝不能作为移除目标。
PnpOutcome TryPnpEject(std::uint32_t disk_number,
                       const std::wstring& volume_hint) {
    PnpOutcome outcome;

    DEVINST node = storage::FindDiskDevInst(disk_number);
    if (node == 0) {
        const DEVINST volume_node = FindVolumeDevInst(volume_hint);
        if (volume_node != 0) {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, volume_node, 0) == CR_SUCCESS) {
                node = parent;
            }
        }
    }
    if (node == 0) {
        USB_LOG_WARN(L"eject", L"PnP 定位失败：disk=%u 找不到设备节点", disk_number);
        return outcome;
    }
    outcome.node_found = true;
    USB_LOG_INFO(L"eject", L"PnP 起点：disk=%u devinst=%lu", disk_number,
                 static_cast<unsigned long>(node));

    DEVINST chain[4] = {};
    int count = 0;
    while (node != 0 && count < 4) {
        const std::wstring id = storage::DeviceInstanceId(node);
        if (id.empty()) break;
        if (storage::IsBusRoot(id)) {
            USB_LOG_INFO(L"eject", L"上溯停止：到达总线根 id=%s", id.c_str());
            break;
        }
        // 遇到 Hub 立即停止：它的上层不是这台设备，而是端口本身。
        if (IsUsbHubNode(node)) {
            USB_LOG_INFO(L"eject",
                         L"上溯停止：遇到 USB Hub node=%lu id=%s（不作为移除目标）",
                         static_cast<unsigned long>(node), id.c_str());
            break;
        }
        chain[count++] = node;
        USB_LOG_DEBUG(L"eject", L"上溯节点：node=%lu id=%s",
                      static_cast<unsigned long>(node), id.c_str());
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    if (count == 0) return outcome;

    // 只移除最上层的 USB 设备节点（U 盘/读卡器本身）。SATA、控制器等
    // 非 USB 节点不调用移除，否则同样可能留下代码 47 的残留状态。
    const DEVINST target = chain[count - 1];
    const std::wstring target_id = storage::DeviceInstanceId(target);
    if (!IsUsbDeviceNode(target_id)) {
        USB_LOG_WARN(L"eject", L"跳过 PnP 移除：目标不是 USB 设备节点 id=%s",
                     target_id.c_str());
        return outcome;
    }
    USB_LOG_INFO(L"eject", L"PnP 移除目标：node=%lu id=%s",
                 static_cast<unsigned long>(target), target_id.c_str());

    wchar_t veto_text[256] = {};
    PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
    outcome.attempted = true;
    const CONFIGRET result =
        CM_Request_Device_EjectW(target, &veto_type, veto_text, 255, 0);
    if (result == CR_SUCCESS) {
        USB_LOG_INFO(L"eject", L"CM_Request_Device_Eject 成功");
        outcome.ok = true;
        return outcome;
    }
    if (result == CR_REMOVE_VETOED) {
        USB_LOG_WARN(L"eject",
                     L"CM_Request_Device_Eject 被否决：type=%d name=%s",
                     static_cast<int>(veto_type), veto_text);
        outcome.vetoed = true;
        outcome.veto_type = veto_type;
        outcome.veto_name = veto_text;
        return outcome;
    }
    USB_LOG_WARN(L"eject", L"CM_Request_Device_Eject 失败：CR=0x%08lX",
                 static_cast<unsigned long>(result));
    outcome.last_error = result;
    return outcome;
}

MediaOutcome MediaEject(const std::wstring& volume_path) {
    MediaOutcome outcome;
    HANDLE volume = CreateFileW(
        storage::OpenableVolumePath(volume_path).c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        outcome.error = GetLastError();
        outcome.busy = outcome.error == ERROR_ACCESS_DENIED ||
                       outcome.error == ERROR_SHARING_VIOLATION;
        USB_LOG_WARN(L"eject", L"介质弹出：打开卷失败 error=0x%08lX",
                     static_cast<unsigned long>(outcome.error));
        return outcome;
    }

    DWORD returned = 0;
    bool locked = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                            &returned, nullptr)) {
            locked = true;
            break;
        }
        Sleep(60);
    }
    if (!locked) {
        outcome.error = GetLastError();
        outcome.busy = outcome.error == ERROR_ACCESS_DENIED ||
                       outcome.error == ERROR_LOCK_VIOLATION ||
                       outcome.error == ERROR_SHARING_VIOLATION;
        USB_LOG_WARN(L"eject", L"介质弹出：锁定卷失败 error=0x%08lX",
                     static_cast<unsigned long>(outcome.error));
        CloseHandle(volume);
        return outcome;
    }

    DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                    &returned, nullptr);
    outcome.ok = DeviceIoControl(volume, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0,
                                 nullptr, 0, &returned, nullptr) != FALSE;
    if (!outcome.ok) outcome.error = GetLastError();
    USB_LOG_INFO(L"eject", L"IOCTL_STORAGE_EJECT_MEDIA 结果=%d error=0x%08lX",
                 outcome.ok ? 1 : 0,
                 static_cast<unsigned long>(outcome.error));
    CloseHandle(volume);
    return outcome;
}

// 先锁定并卸载所有分区，再请求 PnP 移除；需要管理员权限。
ForceOutcome ForceEject(std::uint32_t disk_number,
                        const std::vector<EjectTarget>& targets) {
    ForceOutcome outcome;

    std::vector<HANDLE> opened;
    auto close_all = [&opened]() {
        for (HANDLE handle : opened) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
        opened.clear();
    };

    for (const auto& target : targets) {
        HANDLE volume = CreateFileW(
            storage::OpenableVolumePath(target.volume_path).c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (volume == INVALID_HANDLE_VALUE) {
            outcome.error = GetLastError();
            USB_LOG_WARN(L"eject", L"强制路径：打开卷失败 letter=%c error=0x%08lX",
                         target.letter ? target.letter : L'?',
                         static_cast<unsigned long>(outcome.error));
            close_all();
            if (outcome.error == ERROR_ACCESS_DENIED) {
                outcome.denied = true;
            } else if (outcome.error == ERROR_NOT_READY ||
                       outcome.error == ERROR_NO_MEDIA_IN_DRIVE) {
                outcome.error = ERROR_NOT_READY;
            }
            return outcome;
        }

        DWORD returned = 0;
        bool locked = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr,
                                0, &returned, nullptr)) {
                locked = true;
                break;
            }
            Sleep(60);
        }
        if (!locked) {
            outcome.error = GetLastError();
            USB_LOG_WARN(L"eject", L"强制路径：锁定卷失败 letter=%c error=0x%08lX",
                         target.letter ? target.letter : L'?',
                         static_cast<unsigned long>(outcome.error));
            CloseHandle(volume);
            close_all();
            outcome.busy = outcome.error == ERROR_ACCESS_DENIED ||
                           outcome.error == ERROR_LOCK_VIOLATION ||
                           outcome.error == ERROR_SHARING_VIOLATION;
            return outcome;
        }

        DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                        &returned, nullptr);
        USB_LOG_DEBUG(L"eject", L"强制路径：卷已锁定并卸载 letter=%c",
                      target.letter ? target.letter : L'?');
        PREVENT_MEDIA_REMOVAL prevent = {};
        prevent.PreventMediaRemoval = TRUE;
        DeviceIoControl(volume, IOCTL_STORAGE_MEDIA_REMOVAL, &prevent,
                        sizeof(prevent), nullptr, 0, &returned, nullptr);
        opened.push_back(volume);
    }

    // 先释放自己的锁定句柄，否则 PnP 移除会被自身句柄否决（PNP_VetoOutstandingOpen）。
    // 卷已经完成锁定 + 卸载，关闭句柄后立即请求移除即可。
    close_all();

    const PnpOutcome pnp = TryPnpEject(
        disk_number, targets.empty() ? L"" : targets.front().volume_path);

    if (!pnp.attempted) {
        // 找到了设备节点但没有可安全移除的 USB 目标（例如非 USB 磁盘）：
        // 不做任何「准备移除」，直接按不支持处理，避免留下代码 47 残留。
        if (pnp.node_found) outcome.error = ERROR_NOT_SUPPORTED;
        USB_LOG_WARN(L"eject", L"强制路径：无可用 PnP 目标（node_found=%d）",
                     pnp.node_found ? 1 : 0);
        return outcome;
    }
    if (pnp.ok) {
        const bool gone = storage::WaitDiskGone(disk_number, 4000);
        USB_LOG_INFO(L"eject", L"强制路径：PnP 移除成功，设备消失=%d",
                     gone ? 1 : 0);
        if (gone) {
            outcome.ok = true;
        } else {
            outcome.pending_removal = true;
        }
        return outcome;
    }
    if (pnp.vetoed) {
        outcome.busy = IsOccupancyVeto(pnp.veto_type);
        outcome.error = ERROR_BUSY;
        return outcome;
    }
    outcome.error = ERROR_GEN_FAILURE;
    return outcome;
}

EjectResult Result(EjectStatus status, std::wstring message) {
    EjectResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

EjectResult VetoResult(PNP_VETO_TYPE type, const std::wstring& name,
                       const std::wstring& device) {
    if (IsOccupancyVeto(type)) {
        return Result(EjectStatus::Busy,
                      device + L" 正在被占用：" + VetoMessage(type, name));
    }
    if (type == PNP_VetoNonDisableable ||
        type == PNP_VetoInsufficientPower) {
        return Result(EjectStatus::Unsupported,
                      device + L" 不支持软件移除：" + VetoMessage(type, name));
    }
    return Result(EjectStatus::Failed,
                  device + L" 无法移除：" + VetoMessage(type, name));
}

std::wstring DeviceLabel(const std::wstring& title) {
    return title.empty() ? L"该设备" : L"「" + title + L"」";
}

std::wstring SuccessMessage(const std::wstring& title) {
    return L"已安全弹出" + (title.empty() ? L"设备" : L"「" + title + L"」") +
           L"，现在可以拔下设备了。";
}

EjectResult FailureFromError(DWORD error, const std::wstring& device) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
            return Result(EjectStatus::AccessDenied,
                          L"权限不足，无法锁定该设备的分区。请尝试管理员模式。");
        case ERROR_NOT_READY:
        case ERROR_NO_MEDIA_IN_DRIVE:
            return Result(EjectStatus::NotFound,
                          device + L" 已不在位，可能已被拔出。");
        case ERROR_INVALID_FUNCTION:
        case ERROR_NOT_SUPPORTED:
            return Result(
                EjectStatus::Unsupported,
                L"该设备不支持软件弹出。确认没有程序读写后可直接拔出。");
        default:
            return Result(EjectStatus::Failed,
                          L"弹出失败（错误码 " + std::to_wstring(error) +
                              L"）。请关闭占用程序后重试。");
    }
}

}  // namespace

EjectResult EjectDevice(std::uint32_t disk_number, const std::wstring& title,
                        const std::vector<EjectTarget>& targets) {
    const std::wstring device = DeviceLabel(title);
    USB_LOG_INFO(L"eject", L"开始弹出：disk=%u 分区数=%zu title=%s", disk_number,
                 targets.size(), title.c_str());
    if (targets.empty()) {
        return Result(EjectStatus::NotFound,
                      device + L" 没有可弹出的分区，可能已被拔出。");
    }

    // 1. 首选 PnP 移除，与资源管理器「弹出」走同一条系统路径。
    const PnpOutcome pnp = TryPnpEject(disk_number, targets.front().volume_path);
    if (pnp.ok) {
        const bool gone = storage::WaitDiskGone(disk_number, 5000);
        USB_LOG_INFO(L"eject", L"PnP 移除后等待设备消失：%d", gone ? 1 : 0);
        if (gone) {
            return Result(EjectStatus::Ok, SuccessMessage(title));
        }
        // 移除请求已被接受，只是设备还在完成断开；此时再走介质/强制路径
        // 会打开一个正在被移除的卷，反而得到误导性的“未就绪”。
        return Result(
            EjectStatus::Failed,
            device + L" 已接受移除请求，但设备仍在移除中，请稍后再试。");
    }

    // 明确被打开的文件否决：不必再尝试其他路径，立即进入占用处理。
    if (pnp.vetoed && IsOccupancyVeto(pnp.veto_type)) {
        USB_LOG_INFO(L"eject", L"PnP 被占用否决：type=%d",
                     static_cast<int>(pnp.veto_type));
        return VetoResult(pnp.veto_type, pnp.veto_name, device);
    }

    // 2. 介质弹出：读卡器与部分 U 盘只支持这一条路径。
    const MediaOutcome media = MediaEject(targets.front().volume_path);
    if (media.ok) {
        // 介质弹出语义：读卡器等设备本体可以保留，只要求介质确实不存在，
        // 不能再用盘符消失来判断。
        const bool gone = storage::WaitMediaGone(disk_number, 3000);
        USB_LOG_INFO(L"eject", L"介质弹出后等待介质消失：%d", gone ? 1 : 0);
        if (gone) {
            return Result(EjectStatus::Ok, SuccessMessage(title));
        }
        return Result(
            EjectStatus::Failed,
            device +
                L" 未能真正弹出介质：系统仍检测到介质存在。Windows 可能把该"
                L"移动硬盘识别为固定磁盘，可在设备属性中启用「快速删除」后"
                L"重试。");
    }

    // 连设备节点都找不到：设备多半已经拔出，直接按不在位处理。
    if (!pnp.attempted && !pnp.node_found) {
        USB_LOG_INFO(L"eject", L"结束：设备节点不存在，按已拔出处理");
        return Result(EjectStatus::NotFound,
                      device + L" 已不在位，可能已被拔出。");
    }

    // 没有可安全移除的 USB 设备节点（例如 SATA 磁盘、控制器）：不做锁定/
    // 卸载与移除请求，明确报不支持，避免把节点留在「已准备移除」的状态。
    if (!pnp.attempted && pnp.node_found) {
        USB_LOG_WARN(L"eject", L"结束：无安全移除目标，报不支持");
        return Result(EjectStatus::Unsupported,
                      device + L" 不支持安全移除：未找到可移除的 USB 设备节点。");
    }

    // 3. 强制路径：逐个锁定并卸载分区，再请求 PnP 移除。
    const ForceOutcome force = ForceEject(disk_number, targets);
    USB_LOG_INFO(L"eject", L"强制路径结果：ok=%d pending=%d denied=%d busy=%d error=0x%08lX",
                 force.ok ? 1 : 0, force.pending_removal ? 1 : 0,
                 force.denied ? 1 : 0, force.busy ? 1 : 0,
                 static_cast<unsigned long>(force.error));
    if (force.ok) return Result(EjectStatus::Ok, SuccessMessage(title));
    if (force.pending_removal) {
        return Result(
            EjectStatus::Failed,
            device + L" 已接受移除请求，但设备仍在移除中，请稍后再试。");
    }
    if (force.denied) {
        return Result(EjectStatus::AccessDenied,
                      L"权限不足：无法锁定 " +
                          storage::VolumeDisplayName(
                              targets.front().letter) +
                          L" 分区。请使用管理员模式重试。");
    }
    if (force.busy) {
        return Result(EjectStatus::Busy,
                      device + L" 正在被占用：分区仍被其他程序读写。");
    }

    // 4. 汇总各个阶段的错误，给出最接近原因的结果。
    if (media.busy) {
        return Result(EjectStatus::Busy,
                      device + L" 正在被占用：分区仍被其他程序读写。");
    }
    if (pnp.vetoed) {
        return VetoResult(pnp.veto_type, pnp.veto_name, device);
    }
    if (media.error != 0) return FailureFromError(media.error, device);
    if (pnp.last_error != CR_SUCCESS) {
        return Result(EjectStatus::Failed,
                      L"弹出失败（配置管理器错误码 " +
                          std::to_wstring(force.error != 0 ? force.error
                                                           : pnp.last_error) +
                          L"）。");
    }
    USB_LOG_WARN(
        L"eject", L"结束：media.error=0x%08lX pnp.last_error=0x%08lX force.error=0x%08lX",
        static_cast<unsigned long>(media.error),
        static_cast<unsigned long>(pnp.last_error),
        static_cast<unsigned long>(force.error));
    return FailureFromError(force.error, device);
}

EjectResult EjectAfterRelease(std::uint32_t disk_number,
                              const std::wstring& title,
                              const std::vector<wchar_t>& letters) {
    std::vector<EjectTarget> targets;
    for (wchar_t letter : letters) {
        if (letter == 0) continue;
        wchar_t root[4] = {letter, L':', L'\\', 0};
        wchar_t volume_name[MAX_PATH] = {};
        if (!GetVolumeNameForVolumeMountPointW(root, volume_name, MAX_PATH)) {
            continue;
        }
        targets.push_back(
            EjectTarget{storage::OpenableVolumePath(volume_name), letter});
    }
    if (targets.empty()) {
        return Result(EjectStatus::NotFound,
                      L"没有找到可弹出的分区，设备可能已被拔出。");
    }
    return EjectDevice(disk_number, title, targets);
}

}  // namespace usb
