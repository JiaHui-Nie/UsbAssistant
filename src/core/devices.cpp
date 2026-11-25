#include "core/devices.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/storage.h"
#include "core/text.h"

namespace usb {
namespace {

struct VolumeProbe {
    std::uint32_t disk_number = 0;
    int bus_type = 0;
    bool removable_media = false;
    bool solid_state = false;
    std::uint64_t disk_size = 0;
    std::uint64_t start_offset = 0;
    std::wstring vendor;
    std::wstring product;
};

std::wstring DescriptorString(const char* base, unsigned int offset,
                              unsigned int limit) {
    if (offset == 0 || offset >= limit) return L"";
    std::string text(base + offset);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    if (text.empty()) return L"";

    const int wide =
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (wide <= 1) return L"";
    std::wstring result(static_cast<std::size_t>(wide - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, result.data(), wide);
    return result;
}

bool ProbeVolume(const std::wstring& volume_name, VolumeProbe* probe) {
    HANDLE volume = CreateFileW(storage::OpenableVolumePath(volume_name).c_str(),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return false;

    DWORD returned = 0;
    STORAGE_DEVICE_NUMBER number = {};
    if (!DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                         &number, sizeof(number), &returned, nullptr) ||
        number.DeviceType != FILE_DEVICE_DISK) {
        CloseHandle(volume);
        return false;
    }
    probe->disk_number = number.DeviceNumber;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::vector<char> buffer(4096);
    if (DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query,
                        sizeof(query), buffer.data(),
                        static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        auto* descriptor =
            reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
        probe->bus_type = static_cast<int>(descriptor->BusType);
        probe->removable_media = descriptor->RemovableMedia != FALSE;
        probe->vendor =
            DescriptorString(buffer.data(), descriptor->VendorIdOffset,
                             static_cast<unsigned int>(returned));
        probe->product =
            DescriptorString(buffer.data(), descriptor->ProductIdOffset,
                             static_cast<unsigned int>(returned));
    }

    if (DeviceIoControl(volume, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                        buffer.data(), static_cast<DWORD>(buffer.size()),
                        &returned, nullptr)) {
        auto* geometry = reinterpret_cast<DISK_GEOMETRY_EX*>(buffer.data());
        probe->disk_size =
            static_cast<std::uint64_t>(geometry->DiskSize.QuadPart);
    }

    STORAGE_PROPERTY_QUERY penalty_query = {};
    penalty_query.PropertyId = StorageDeviceSeekPenaltyProperty;
    penalty_query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty = {};
    if (DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &penalty_query,
                        sizeof(penalty_query), &penalty, sizeof(penalty),
                        &returned, nullptr)) {
        probe->solid_state = penalty.IncursSeekPenalty == FALSE;
    }

    PARTITION_INFORMATION_EX partition = {};
    if (DeviceIoControl(volume, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0,
                        &partition, sizeof(partition), &returned, nullptr)) {
        probe->start_offset = static_cast<std::uint64_t>(
            partition.StartingOffset.QuadPart);
    } else {
        std::vector<BYTE> extents_buffer(
            sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 8);
        if (DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, extents_buffer.data(),
                            static_cast<DWORD>(extents_buffer.size()),
                            &returned, nullptr)) {
            auto* extents =
                reinterpret_cast<VOLUME_DISK_EXTENTS*>(extents_buffer.data());
            if (extents->NumberOfDiskExtents > 0) {
                probe->start_offset = static_cast<std::uint64_t>(
                    extents->Extents[0].StartingOffset.QuadPart);
            }
        }
    }

    CloseHandle(volume);
    return true;
}

Partition ReadPartition(const std::wstring& volume_name,
                        const VolumeProbe& probe) {
    Partition part;
    part.volume_path = storage::OpenableVolumePath(volume_name);
    part.start_offset = probe.start_offset;

    const std::wstring root = storage::FirstDriveLetter(volume_name);
    if (!root.empty()) {
        part.letter = root[0];
        part.root = root;
    }

    ULARGE_INTEGER total = {};
    ULARGE_INTEGER free_bytes = {};
    ULARGE_INTEGER available = {};
    if (GetDiskFreeSpaceExW(volume_name.c_str(), &available, &total,
                            &free_bytes)) {
        part.total = total.QuadPart;
        part.free_bytes = free_bytes.QuadPart;
    }

    wchar_t label[128] = {};
    wchar_t filesystem[64] = {};
    DWORD serial = 0;
    DWORD max_component = 0;
    DWORD flags = 0;
    if (GetVolumeInformationW(volume_name.c_str(), label, 128, &serial,
                              &max_component, &flags, filesystem, 64)) {
        part.label = label;
        part.filesystem = filesystem;
    }
    if (part.filesystem.empty()) part.filesystem = L"未知文件系统";
    return part;
}

bool SameDisk(const Device& device, std::uint32_t disk_number) {
    return device.disk_number == disk_number;
}

std::wstring FallbackPartitionLabel(const Partition& part, bool removable_media) {
    if (part.kind == PartitionKind::Esp) return L"ESP";
    if (part.kind == PartitionKind::Msr) return L"MSR";
    if (!part.label.empty()) return part.label;
    // 卷标为空时：本地磁盘显示「本地磁盘」；可移动盘沿用图标使用的
    // removable_media 区分，U 盘与移动硬盘各用原占位名。
    if (!part.root.empty() &&
        GetDriveTypeW(part.root.c_str()) == DRIVE_FIXED) {
        return L"本地磁盘";
    }
    return removable_media ? L"U 盘" : L"移动硬盘";
}

void SortPartitionsByLetter(std::vector<Partition>* partitions);

void ApplyPartitionTable(Device* device) {
    const auto table = storage::ReadPartitionTable(device->disk_number);
    if (table.empty()) return;

    std::vector<bool> used(device->partitions.size(), false);
    std::vector<Partition> ordered;
    int matched = 0;

    for (const auto& entry : table) {
        Partition part;
        bool hit = false;
        for (std::size_t i = 0; i < device->partitions.size(); ++i) {
            if (used[i] ||
                device->partitions[i].start_offset != entry.start) {
                continue;
            }
            part = std::move(device->partitions[i]);
            used[i] = true;
            hit = true;
            break;
        }
        part.total = entry.length;
        part.start_offset = entry.start;
        part.kind = entry.kind;
        if (hit) {
            ++matched;
        } else {
            // 分区表里有、Windows 没有对应卷的分区（ESP / MSR / 未挂载）。
            part.letter = 0;
            part.root.clear();
            part.volume_path.clear();
            part.free_bytes = 0;
            part.filesystem = L"未知文件系统";
            part.label.clear();
        }
        if (hit) {
            part.label = FallbackPartitionLabel(part, device->removable_media);
        } else if (part.kind == PartitionKind::Esp) {
            part.label = L"ESP";
        } else if (part.kind == PartitionKind::Msr) {
            part.label = L"MSR";
        } else {
            part.label = L"未挂载分区";
        }
        ordered.push_back(std::move(part));
    }

    if (matched == 0) {
        SortPartitionsByLetter(&device->partitions);
        return;  // 偏移对不上（个别 MBR 磁盘）：保留卷信息。
    }

    for (std::size_t i = 0; i < device->partitions.size(); ++i) {
        if (!used[i]) ordered.push_back(std::move(device->partitions[i]));
    }
    device->partitions.swap(ordered);
}

void SortPartitionsByLetter(std::vector<Partition>* partitions) {
    std::sort(partitions->begin(), partitions->end(),
              [](const Partition& left, const Partition& right) {
                  if (left.letter == 0) return false;
                  if (right.letter == 0) return true;
                  return left.letter < right.letter;
              });
}

}  // namespace

std::vector<Device> EnumerateDevices(const DeviceFilter& filter) {
    std::vector<Device> devices;
    USB_LOG_INFO(L"devices", L"开始枚举卷；过滤 hidden=%d esp=%d msr=%d other=%d",
                 filter.include_hidden ? 1 : 0, filter.include_esp ? 1 : 0,
                 filter.include_msr ? 1 : 0,
                 filter.include_other_hidden ? 1 : 0);

    wchar_t volume_name[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(volume_name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) {
        USB_LOG_WARN(L"devices", L"FindFirstVolume 失败 error=0x%08lX",
                     static_cast<unsigned long>(GetLastError()));
        return devices;
    }

    do {
        VolumeProbe probe;
        if (!ProbeVolume(volume_name, &probe)) {
            USB_LOG_DEBUG(L"devices", L"跳过卷（无法读取设备信息）%s",
                          volume_name);
            continue;
        }
        if (probe.bus_type != BusTypeUsb) {  // 只关心 USB 总线
            USB_LOG_DEBUG(L"devices", L"跳过卷（bus=%d）%s",
                          static_cast<int>(probe.bus_type), volume_name);
            continue;
        }

        // 空读卡器没有介质：跳过该卷；如果这块磁盘没有任何可用分区，
        // 最终整块设备都不会显示（避免列出空槽位）。
        ULARGE_INTEGER total = {};
        ULARGE_INTEGER free_bytes = {};
        ULARGE_INTEGER available = {};
        const BOOL has_size =
            GetDiskFreeSpaceExW(volume_name, &available, &total, &free_bytes);
        if (!has_size) {
            const DWORD error = GetLastError();
            if (error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) {
                USB_LOG_INFO(L"devices",
                             L"跳过卷（无介质）error=0x%08lX %s",
                             static_cast<unsigned long>(error), volume_name);
                continue;
            }
        }

        Partition part = ReadPartition(volume_name, probe);
        part.free_bytes = has_size ? free_bytes.QuadPart : 0;
        part.total = has_size ? total.QuadPart : 0;
        USB_LOG_DEBUG(L"devices",
                      L"卷：disk=%u letter=%c fs=%s total=%llu volume=%s",
                      probe.disk_number,
                      part.letter ? part.letter : L'-',
                      part.filesystem.c_str(),
                      static_cast<unsigned long long>(part.total),
                      volume_name);

        Device* device = nullptr;
        for (auto& candidate : devices) {
            if (SameDisk(candidate, probe.disk_number)) {
                device = &candidate;
                break;
            }
        }
        if (device == nullptr) {
            Device fresh;
            fresh.disk_number = probe.disk_number;
            fresh.removable_media = probe.removable_media;
            fresh.solid_state = probe.solid_state;
            fresh.physical_size = probe.disk_size;
            std::wstring pnp_name = storage::PnpDisplayName(probe.disk_number);
            if (pnp_name.empty()) {
                fresh.title = text::DeviceTitle(probe.vendor, probe.product,
                                                probe.removable_media);
            } else {
                fresh.title = std::move(pnp_name);
            }
            devices.push_back(std::move(fresh));
            device = &devices.back();
        }
        device->removable_media =
            device->removable_media || probe.removable_media;
        device->solid_state = device->solid_state || probe.solid_state;
        device->partitions.push_back(std::move(part));
    } while (FindNextVolumeW(find, volume_name, MAX_PATH));

    FindVolumeClose(find);

    for (auto& device : devices) {
        ApplyPartitionTable(&device);

        device.partitions.erase(
            std::remove_if(device.partitions.begin(), device.partitions.end(),
                           [&filter, &device](const Partition& part) {
                               if (part.letter != 0) return false;
                               const wchar_t* reason = nullptr;
                               if (!filter.include_hidden) {
                                   reason = L"无盘符总开关关闭";
                               } else {
                                   switch (part.kind) {
                                       case PartitionKind::Esp:
                                           if (!filter.include_esp) {
                                               reason = L"ESP 未勾选";
                                           }
                                           break;
                                       case PartitionKind::Msr:
                                           if (!filter.include_msr) {
                                               reason = L"MSR 未勾选";
                                           }
                                           break;
                                       default:
                                           if (!filter.include_other_hidden) {
                                               reason = L"其他隐藏分区未勾选";
                                           }
                                           break;
                                   }
                               }
                               if (reason) {
                                   USB_LOG_INFO(
                                       L"devices",
                                       L"过滤分区：disk=%u kind=%d reason=%s",
                                       device.disk_number,
                                       static_cast<int>(part.kind), reason);
                                   return true;
                               }
                               return false;
                           }),
            device.partitions.end());
        if (device.partitions.empty()) {
            USB_LOG_INFO(L"devices", L"跳过设备 disk=%u（没有可见分区）",
                         device.disk_number);
            continue;
        }

        device.total = 0;
        device.free_bytes = 0;
        for (const auto& part : device.partitions) {
            device.free_bytes += part.free_bytes;
        }
        device.total = device.physical_size;
        if (device.total == 0) {
            for (const auto& part : device.partitions) device.total += part.total;
        }
        USB_LOG_INFO(L"devices", L"设备：disk=%u title=%s 分区=%zu",
                     device.disk_number, device.title.c_str(),
                     device.partitions.size());
    }

    devices.erase(
        std::remove_if(devices.begin(), devices.end(),
                       [](const Device& device) {
                           return device.partitions.empty();
                       }),
        devices.end());

    std::sort(devices.begin(), devices.end(),
              [](const Device& left, const Device& right) {
                  const wchar_t left_letter =
                      left.partitions.empty() ? 0 : left.partitions.front().letter;
                  const wchar_t right_letter = right.partitions.empty()
                                                   ? 0
                                                   : right.partitions.front().letter;
                  if (left_letter == 0) return false;
                  if (right_letter == 0) return true;
                  return left_letter < right_letter;
              });

    USB_LOG_INFO(L"devices", L"枚举完成：设备数=%zu", devices.size());
    return devices;
}

}  // namespace usb
