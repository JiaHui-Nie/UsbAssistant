// 卷占用检测与解除。通过系统句柄表反查正在打开该卷的进程。
#pragma once

#include <vector>

#include "core/models.h"

namespace usb {

// complete 非空时用于区分“扫描失败”与“确实没有占用”：失败时置 false。
std::vector<Occupant> FindOccupants(const std::vector<wchar_t>& letters,
                                    bool* complete = nullptr);

ReleaseReport ReleaseOccupants(const std::vector<DWORD>& pids,
                                const std::vector<wchar_t>& letters);

// 便宜的存活检查：只查询进程是否仍在运行，不做句柄快照。
// 无法查询（权限不足等）时保守地视为存活，避免误清列表。
bool OccupantsStillAlive(const std::vector<Occupant>& occupants);

}  // namespace usb
