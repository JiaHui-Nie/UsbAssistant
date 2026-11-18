// 核心数据模型：只描述“是什么”，不包含任何界面状态或持久化逻辑。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace usb {

enum class PartitionKind { Normal, Esp, Msr };

struct Partition {
    wchar_t letter = 0;         // 盘符；无盘符（隐藏分区）时为 0
    std::wstring root;          // "F:\"；无盘符时为空
    std::wstring volume_path;   // "\\?\Volume{...}"；已去掉末尾反斜杠
    std::wstring label;         // 卷标，缺省时是「U 盘」/「本地磁盘」/「ESP」/「MSR」
    std::wstring filesystem;    // NTFS / exFAT / FAT32 / 未知文件系统
    PartitionKind kind = PartitionKind::Normal;
    std::uint64_t total = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t start_offset = 0;
};

struct Device {
    std::uint32_t disk_number = 0;
    std::wstring title;         // 设备友好名称（已清洗通用词）
    bool removable_media = false;
    bool solid_state = false;
    std::vector<Partition> partitions;
    std::uint64_t physical_size = 0;  // 整块物理磁盘容量（几何信息）
    std::uint64_t total = 0;          // 对界面展示的总容量
    std::uint64_t free_bytes = 0;     // 各可用分区剩余空间之和

    bool has_letter() const;
};

struct DeviceFilter {
    bool include_hidden = false;  // 是否列出无盘符分区（ESP / MSR / 未挂载）
    bool include_esp = true;      // 仅在 include_hidden 时生效
    bool include_msr = true;
    bool include_other_hidden = true;
};

// 正在占用卷的进程。图标随对象生命周期释放。
class Occupant {
public:
    Occupant() = default;
    ~Occupant();
    Occupant(const Occupant&) = delete;
    Occupant& operator=(const Occupant&) = delete;
    Occupant(Occupant&& other) noexcept;
    Occupant& operator=(Occupant&& other) noexcept;

    DWORD pid = 0;
    std::wstring name;  // 进程显示名（文件描述优先）
    std::wstring path;  // 完整可执行文件路径
    HICON icon = nullptr;
};

enum class EjectStatus {
    Ok = 0,
    Busy,          // 仍被程序占用
    AccessDenied,  // 需要管理员权限
    NotFound,      // 设备已不在
    Unsupported,   // 不支持软件弹出
    Failed,
};

struct EjectTarget {
    std::wstring volume_path;  // "\\?\Volume{...}"
    wchar_t letter = 0;
};

struct EjectResult {
    EjectStatus status = EjectStatus::Failed;
    std::wstring message;

    bool ok() const { return status == EjectStatus::Ok; }
};

struct ReleaseReport {
    int closed = 0;           // 被结束的普通进程
    int denied = 0;           // 权限不足
    int skipped = 0;          // 系统关键进程或其他原因跳过
    int explorer = 0;         // 跳过的 explorer 进程
    int explorer_windows = 0; // 被关闭的资源管理器窗口

    int released() const { return closed + explorer_windows; }
};

bool IsProcessElevated();

}  // namespace usb
