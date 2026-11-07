#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 占用了某个卷（U 盘 / 移动硬盘分区）的进程
struct VolumeOccupant {
    DWORD pid = 0;
    std::wstring name; // 与任务管理器一致的显示名（文件说明，缺失时用文件名）
    std::wstring path; // 进程可执行文件的完整路径
    HICON icon = nullptr;
};

// 枚举所有打开了这些盘符上文件的进程（按进程去重）。
// 通过 NtQuerySystemInformation 枚举全系统句柄并比对卷的设备路径实现。
std::vector<VolumeOccupant> EnumerateVolumeOccupants(
    const std::vector<wchar_t>& letters);

// 结束给定进程（跳过自身与系统进程），返回成功结束的数量。
int TerminateOccupants(const std::vector<DWORD>& pids);

// 释放图标
void FreeOccupants(std::vector<VolumeOccupant>* list);
