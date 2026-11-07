#pragma once

#include <string>
#include <vector>

// 一个分区（卷）
struct PartitionInfo {
    wchar_t letter = 0;         // 盘符；无盘符时为 0
    std::wstring root;          // "F:\"；无盘符时为空
    std::wstring volumePath;    // "\\?\Volume{...}\"
    std::wstring name;          // 卷标；无卷标时显示盘符
    std::wstring fs;            // NTFS / exFAT / FAT32 / 未知文件系统
    unsigned long long total = 0;
    unsigned long long freeBytes = 0;
    bool openable = false;      // 仅资源管理器支持的文件系统才允许打开

    float hoverOpen = 0.0f;     // 界面动画状态
};

// 一个物理设备（同一块磁盘上的多个分区归为一组）
struct DeviceInfo {
    unsigned int diskNumber = 0;
    std::wstring model;         // 硬件型号，如 "SanDisk Cruzer Blade"
    std::wstring title;         // 卡片标题（已去掉 USB/Generic 等通用字样）
    bool removableMedia = false; // true=U 盘/存储卡，false=移动硬盘
    std::vector<PartitionInfo> parts;
    unsigned long long total = 0;
    unsigned long long freeBytes = 0;

    bool ejecting = false;
    float hoverEject = 0.0f;
};

// 枚举 USB 总线的可移动存储设备，按物理磁盘分组。
std::vector<DeviceInfo> EnumerateRemovableDevices();

// 该文件系统是否可被 Windows 资源管理器打开。
bool IsOpenableFileSystem(const std::wstring& fs);

std::wstring FormatBytes(unsigned long long bytes);
