#pragma once

#include <string>
#include <vector>

enum class EjectStatus {
    Ok,           // 已安全弹出
    Busy,         // 设备被占用
    AccessDenied, // 权限不足
    NotFound,     // 设备不存在
    Unsupported,  // 设备不支持软件弹出
    Failed        // 其他失败
};

struct EjectOutcome {
    EjectStatus status = EjectStatus::Failed;
    std::wstring message;
};

struct EjectVolumeTarget {
    std::wstring path; // "\\?\Volume{...}\"
    wchar_t letter = 0;
};

// 对整个物理设备执行安全弹出：
// 锁定并卸载其所有分区 -> CM_Request_Device_Eject 逐级移除（失败回退 IOCTL_STORAGE_EJECT_MEDIA）。
EjectOutcome EjectDisk(unsigned int diskNumber, const std::wstring& title,
                       const std::vector<EjectVolumeTarget>& volumes);
