// USB 存储设备枚举：以卷为入口，按物理磁盘聚合，并以分区表校正分区列表。
#pragma once

#include <vector>

#include "core/models.h"

namespace usb {

std::vector<Device> EnumerateDevices(const DeviceFilter& filter = {});

}  // namespace usb
