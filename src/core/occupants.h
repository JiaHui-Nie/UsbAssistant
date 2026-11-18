// 卷占用检测与解除。通过系统句柄表反查正在打开该卷的进程。
#pragma once

#include <vector>

#include "core/models.h"

namespace usb {

std::vector<Occupant> FindOccupants(const std::vector<wchar_t>& letters);

ReleaseReport ReleaseOccupants(const std::vector<DWORD>& pids,
                                const std::vector<wchar_t>& letters);

}  // namespace usb
