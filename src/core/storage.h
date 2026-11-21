// 磁盘 / 卷相关的 Win32 原语。设备枚举、弹出、占用检测共用这一层，
// 避免同一段 SetupAPI / CfgMgr32 代码在多处复制。
#pragma once

#include <windows.h>
#include <cfgmgr32.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/models.h"

namespace usb::storage {

struct PartitionEntry {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    PartitionKind kind = PartitionKind::Normal;
};

// 给无盘符分区分配 / 取消盘符，语义等同于磁盘管理里的“更改驱动器号”。
enum class MountStatus {
    Ok = 0,
    AccessDenied,  // 需要管理员权限
    NotFound,      // 卷已不在
    NoFreeLetter,  // 没有可用盘符
    Unsupported,   // 该分区不支持分配盘符
    Failed,
};

struct MountResult {
    MountStatus status = MountStatus::Failed;
    wchar_t letter = 0;
    std::wstring message;

    bool ok() const { return status == MountStatus::Ok; }
};

MountResult AssignDriveLetter(const std::wstring& volume_path);
MountResult RemoveDriveLetter(wchar_t letter);

// 查询卷当前已分配的盘符；没有则返回 0。用于避免重复分配盘符。
wchar_t FindVolumeDriveLetter(const std::wstring& volume_path);

// 探测当前进程能否列出该目录（与资源管理器浏览同等权限）。
// 返回 ERROR_SUCCESS 表示可浏览；否则返回 Win32 错误码。
DWORD ProbeDirectoryAccess(const std::wstring& path);

// CreateFile 不接受卷 GUID 路径末尾的反斜杠；其余卷 API 又要求带反斜杠。
std::wstring OpenableVolumePath(std::wstring volume_path);

// 通过 IOCTL_STORAGE_GET_DEVICE_NUMBER 找到物理磁盘对应的设备节点。
DEVINST FindDiskDevInst(std::uint32_t disk_number);
bool IsDiskPresent(std::uint32_t disk_number);
// 介质是否仍然可访问：设备已不存在或报告无介质时为 false。
bool IsMediaPresent(std::uint32_t disk_number);

// 整盘移除：只有磁盘设备节点确实消失才算成功，盘符消失不作为依据。
bool WaitDiskGone(std::uint32_t disk_number, int timeout_ms);
// 介质弹出：只看介质状态，设备本体（读卡器等）可以继续保留。
bool WaitMediaGone(std::uint32_t disk_number, int timeout_ms);

std::vector<PartitionEntry> ReadPartitionTable(std::uint32_t disk_number);

// “设备和打印机”里的友好名称（磁盘节点优先，逐级向上查找）。
std::wstring PnpDisplayName(std::uint32_t disk_number);

std::wstring DeviceInstanceId(DEVINST node);
bool IsBusRoot(const std::wstring& instance_id);

// 盘符 -> NT 设备路径（例如 "\Device\HarddiskVolume3"）。
std::vector<std::wstring> DosDevicePaths(const std::vector<wchar_t>& letters);

// 取卷的第一个盘符挂载点，例如 "F:\"；没有盘符时返回空串。
std::wstring FirstDriveLetter(const std::wstring& volume_name);

std::wstring VolumeDisplayName(wchar_t letter);

}  // namespace usb::storage
