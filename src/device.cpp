#include "device.h"

#include "settings.h"

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// GUID_DEVINTERFACE_DISK，避免对 libuuid 的链接依赖
const GUID kGuidDevInterfaceDisk = {
    0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

// 从 STORAGE_DEVICE_DESCRIPTOR 的偏移量里取 ANSI 型号字符串
std::wstring DescriptorString(const char* base, unsigned int offset, unsigned int limit) {
    if (offset == 0 || offset >= limit) return L"";
    std::string text(base + offset);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    if (text.empty()) return L"";
    int wide = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (wide <= 1) return L"";
    std::wstring out(static_cast<size_t>(wide - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, out.data(), wide);
    return out;
}

// CreateFile 不接受卷 GUID 路径末尾的反斜杠（会返回 ERROR_PATH_NOT_FOUND），
// 而 GetVolumeInformation / GetDiskFreeSpaceEx 又要求带反斜杠的写法，故分别处理。
std::wstring OpenableVolumePath(const std::wstring& volumePath) {
    std::wstring path = volumePath;
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}

bool IsGenericWord(const std::wstring& word) {
    // 以 USB / Generic 开头的词（如 "Generic-"、"USB2.0"）一律视为通用前缀
    if (word.size() >= 3 && _wcsicmp(word.substr(0, 3).c_str(), L"usb") == 0) return true;
    if (word.size() >= 7 &&
        _wcsicmp(word.substr(0, 7).c_str(), L"generic") == 0) {
        return true;
    }

    static const wchar_t* kWords[] = {
        L"usb",   L"generic", L"mass",    L"storage", L"disk",  L"device",
        L"flash", L"removable", L"drive",  L"2.0",     L"3.0",   L"3.1",
        L"3.2"};
    for (const wchar_t* candidate : kWords) {
        if (_wcsicmp(word.c_str(), candidate) == 0) return true;
    }
    return false;
}

// 按空格切分成词
std::vector<std::wstring> SplitTokens(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::wstring JoinTokens(const std::vector<std::wstring>& tokens, size_t begin,
                        size_t end) {
    std::wstring out;
    for (size_t i = begin; i < end; ++i) {
        if (!out.empty()) out += L' ';
        out += tokens[i];
    }
    return out;
}

// 去掉开头连续出现的 USB/Generic/Mass Storage 等通用词
std::wstring StripGenericWords(const std::wstring& text) {
    std::vector<std::wstring> tokens = SplitTokens(text);
    size_t start = 0;
    while (start < tokens.size() && IsGenericWord(tokens[start])) ++start;
    return JoinTokens(tokens, start, tokens.size());
}

// 系统友好名称常见的结尾词，如 "SanDisk Cruzer Blade USB Device"
bool IsTrailingGeneric(const std::wstring& word) {
    return _wcsicmp(word.c_str(), L"device") == 0 ||
           _wcsicmp(word.c_str(), L"usb") == 0 ||
           _wcsicmp(word.c_str(), L"设备") == 0;
}

// 系统友好名称同样需要去掉 "USB Device" 之类的通用前后缀
std::wstring CleanPnpName(const std::wstring& text) {
    std::vector<std::wstring> tokens = SplitTokens(text);
    size_t start = 0;
    while (start < tokens.size() && IsGenericWord(tokens[start])) ++start;
    size_t end = tokens.size();
    while (end > start && IsTrailingGeneric(tokens[end - 1])) --end;
    return JoinTokens(tokens, start, end);
}

// 组合出干净的硬件名称
std::wstring CleanModelName(const std::wstring& vendor, const std::wstring& product,
                            bool removableMedia) {
    std::wstring cleanVendor = StripGenericWords(vendor);
    std::wstring cleanProduct = StripGenericWords(product);
    std::wstring name;
    if (!cleanVendor.empty() && !cleanProduct.empty()) {
        name = cleanVendor + L" " + cleanProduct;
    } else if (!cleanProduct.empty()) {
        name = cleanProduct;
    } else if (!cleanVendor.empty()) {
        name = cleanVendor;
    }
    if (name.empty()) name = removableMedia ? L"可移动磁盘" : L"移动硬盘";
    return name;
}

// 查询卷所在的物理磁盘号、总线类型与硬件型号
bool ProbeVolume(const std::wstring& volumePath, unsigned int* diskNumber,
                 int* busType, std::wstring* vendor, std::wstring* product,
                 bool* removableMedia, unsigned long long* diskSize) {
    std::wstring openPath = OpenableVolumePath(volumePath);
    HANDLE handle = CreateFileW(openPath.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    DWORD returned = 0;
    bool ok = false;

    STORAGE_DEVICE_NUMBER number = {};
    if (DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number,
                        sizeof(number), &returned, nullptr) &&
        number.DeviceType == FILE_DEVICE_DISK) {
        *diskNumber = number.DeviceNumber;
        ok = true;
    }

    if (ok && busType) {
        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        char buffer[1024] = {};
        if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            buffer, sizeof(buffer), &returned, nullptr)) {
            auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
            *busType = static_cast<int>(desc->BusType);
            if (removableMedia) *removableMedia = desc->RemovableMedia != FALSE;
            if (vendor) {
                *vendor = DescriptorString(buffer, desc->VendorIdOffset, returned);
            }
            if (product) {
                *product = DescriptorString(buffer, desc->ProductIdOffset, returned);
            }
        }
    }

    // 卷的磁盘几何信息里 DiskSize 是整块物理磁盘的容量，
    // 不随分区增删变化，且普通权限即可读取（PhysicalDrive 需要管理员）
    if (ok && diskSize) {
        BYTE geometry[sizeof(DISK_GEOMETRY_EX) + 64] = {};
        if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                            geometry, sizeof(geometry), &returned, nullptr)) {
            auto* ex = reinterpret_cast<DISK_GEOMETRY_EX*>(geometry);
            *diskSize = static_cast<unsigned long long>(ex->DiskSize.QuadPart);
        }
    }

    CloseHandle(handle);
    return ok;
}

// 通过磁盘设备号找到物理磁盘的设备节点
DEVINST FindDiskDevInst(unsigned int diskNumber) {
    HDEVINFO set = SetupDiGetClassDevsW(&kGuidDevInterfaceDisk, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    DEVINST found = 0;
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &kGuidDevInterfaceDisk, index,
                                     &interfaceData);
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
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, required,
                                              &required, &infoData)) {
            continue;
        }

        HANDLE handle = CreateFileW(detail->DevicePath, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER number = {};
        DWORD returned = 0;
        bool matched =
            DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                            &number, sizeof(number), &returned, nullptr) &&
            number.DeviceType == FILE_DEVICE_DISK && number.DeviceNumber == diskNumber;
        CloseHandle(handle);
        if (matched) {
            found = infoData.DevInst;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

std::wstring DevNodeProperty(DEVINST node, ULONG property) {
    wchar_t buffer[512] = {};
    ULONG size = sizeof(buffer);
    if (CM_Get_DevNode_Registry_PropertyW(node, property, nullptr, buffer, &size, 0) !=
        CR_SUCCESS) {
        return L"";
    }
    return buffer;
}

// 「设备和打印机」显示的是设备容器的友好名称，即 PnP 设备的 FriendlyName。
// 磁盘节点的名称最贴近（如 "SanDisk Cruzer Blade USB Device"），若为空再向父节点取。
std::wstring PnpDisplayName(unsigned int diskNumber) {
    DEVINST node = FindDiskDevInst(diskNumber);
    if (node == 0) return L"";

    DEVINST chain[4] = {};
    int count = 0;
    while (node != 0 && count < 4) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        std::wstring lower = id;
        for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        if (lower.find(L"root_hub") != std::wstring::npos ||
            lower.find(L"usb\\hub") != std::wstring::npos ||
            lower.find(L"pci\\") != std::wstring::npos) {
            break;
        }
        chain[count++] = node;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }

    for (int index = 0; index < count; ++index) {
        std::wstring name = CleanPnpName(DevNodeProperty(chain[index], CM_DRP_FRIENDLYNAME));
        if (!name.empty()) return name;
    }
    return L"";
}

// 取卷的第一个盘符挂载点，例如 "F:\"
std::wstring FirstDriveLetter(const std::wstring& volumePath) {
    wchar_t mounts[512] = {};
    if (!GetVolumePathNamesForVolumeNameW(volumePath.c_str(), mounts, 512, nullptr)) {
        return L"";
    }
    for (const wchar_t* mount = mounts; *mount != L'\0'; mount += wcslen(mount) + 1) {
        if (wcslen(mount) == 3 && mount[0] >= L'A' && mount[1] == L':' &&
            mount[2] == L'\\') {
            return mount;
        }
    }
    return L"";
}

bool IsUsbBus(int busType) {
    return busType == BusTypeUsb;
}

} // namespace

bool IsOpenableFileSystem(const std::wstring& fs) {
    static const wchar_t* kSupported[] = {L"NTFS", L"FAT", L"FAT32", L"exFAT",
                                          L"ReFS"};
    for (const wchar_t* name : kSupported) {
        if (_wcsicmp(fs.c_str(), name) == 0) return true;
    }
    return false;
}

std::wstring FormatBytes(unsigned long long bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    const double base = CapacityBase() == 1000 ? 1000.0 : 1024.0;
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= base && unit < 4) {
        value /= base;
        ++unit;
    }

    wchar_t number[40];
    if (unit == 0) {
        swprintf(number, 40, L"%llu", bytes);
    } else {
        swprintf(number, 40, L"%.2f", value);
    }
    std::wstring out = number;
    out += L" ";
    out += kUnits[unit];
    return out;
}

std::vector<DeviceInfo> EnumerateRemovableDevices() {
    // 先把所有卷探测出来，再按物理磁盘分组
    struct TempVolume {
        unsigned int disk = 0;
        unsigned long long diskSize = 0;
        int bus = 0; // BusTypeUnknown
        std::wstring vendor;
        std::wstring product;
        bool removableMedia = false;
        PartitionInfo part;
    };

    std::vector<TempVolume> volumes;

    wchar_t volumeName[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(volumeName, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return {};

    do {
        unsigned int diskNumber = 0;
        int busType = 0;
        std::wstring vendor;
        std::wstring product;
        bool removableMedia = false;
        unsigned long long diskSize = 0;
        if (!ProbeVolume(volumeName, &diskNumber, &busType, &vendor, &product,
                         &removableMedia, &diskSize)) {
            continue;
        }

        PartitionInfo part;
        part.volumePath = OpenableVolumePath(volumeName); // 供 CreateFile 使用

        std::wstring root = FirstDriveLetter(volumeName);
        if (!root.empty()) {
            part.letter = root[0];
            part.root = root;
        }

        ULARGE_INTEGER totalBytes = {};
        ULARGE_INTEGER freeBytes = {};
        ULARGE_INTEGER availableBytes = {};
        if (!GetDiskFreeSpaceExW(volumeName, &availableBytes, &totalBytes, &freeBytes)) {
            continue; // 无介质（读卡器空槽）或无法访问
        }
        if (totalBytes.QuadPart == 0) continue;

        part.total = totalBytes.QuadPart;
        part.freeBytes = freeBytes.QuadPart;

        wchar_t label[128] = {};
        wchar_t fs[64] = {};
        DWORD serial = 0;
        DWORD maxComponent = 0;
        DWORD flags = 0;
        if (GetVolumeInformationW(volumeName, label, 128, &serial, &maxComponent, &flags,
                                  fs, 64)) {
            part.name = label;
            part.fs = fs;
        }
        if (part.name.empty()) part.name = L"本地磁盘";
        if (part.fs.empty()) part.fs = L"未知文件系统";
        part.openable = part.letter != 0 && IsOpenableFileSystem(part.fs);

        TempVolume temp;
        temp.disk = diskNumber;
        temp.diskSize = diskSize;
        temp.bus = busType;
        temp.vendor = vendor;
        temp.product = product;
        temp.removableMedia = removableMedia;
        temp.part = std::move(part);
        volumes.push_back(std::move(temp));
    } while (FindNextVolumeW(find, volumeName, MAX_PATH));

    FindVolumeClose(find);

    std::vector<DeviceInfo> devices;
    for (auto& volume : volumes) {
        DeviceInfo* group = nullptr;
        for (auto& device : devices) {
            if (device.diskNumber == volume.disk) {
                group = &device;
                break;
            }
        }
        if (!group) {
            DeviceInfo device;
            device.diskNumber = volume.disk;
            device.model = CleanModelName(volume.vendor, volume.product,
                                          volume.removableMedia);
            device.removableMedia = volume.removableMedia;
            devices.push_back(std::move(device));
            group = &devices.back();
        }
        if (!IsUsbBus(volume.bus)) continue; // 非 USB 总线不纳入
        group->removableMedia = group->removableMedia || volume.removableMedia;
        group->parts.push_back(std::move(volume.part));
    }

    // 丢弃非 USB 总线 / 没有任何分区的设备
    devices.erase(std::remove_if(devices.begin(), devices.end(),
                                 [](const DeviceInfo& device) {
                                     return device.parts.empty();
                                 }),
                  devices.end());

    for (auto& device : devices) {
        // 设备总容量取整块物理磁盘的大小；取不到时退回各分区之和
        device.total = 0;
        device.freeBytes = 0;
        for (const auto& part : device.parts) {
            device.total += part.total;
            device.freeBytes += part.freeBytes;
        }
        for (const auto& volume : volumes) {
            if (volume.disk == device.diskNumber && volume.diskSize > 0) {
                device.total = volume.diskSize;
                break;
            }
        }

        std::sort(device.parts.begin(), device.parts.end(),
                  [](const PartitionInfo& a, const PartitionInfo& b) {
                      if (a.letter == 0) return false;
                      if (b.letter == 0) return true;
                      return a.letter < b.letter;
                  });

        // 优先使用系统「设备和打印机」中的设备友好名称，取不到时退回固件型号
        std::wstring pnpName = PnpDisplayName(device.diskNumber);
        if (!pnpName.empty()) device.model = pnpName;

        if (!device.model.empty()) {
            device.title = device.model;
        } else {
            device.title = device.parts.front().name;
        }
    }

    std::sort(devices.begin(), devices.end(),
              [](const DeviceInfo& a, const DeviceInfo& b) {
                  wchar_t la = a.parts.empty() ? 0 : a.parts.front().letter;
                  wchar_t lb = b.parts.empty() ? 0 : b.parts.front().letter;
                  if (la == 0) return false;
                  if (lb == 0) return true;
                  return la < lb;
              });

    return devices;
}
