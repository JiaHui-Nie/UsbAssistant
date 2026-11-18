// 安全弹出流程：PnP 移除 → 介质弹出 → 强制锁定/卸载后重试。
#pragma once

#include <string>
#include <vector>

#include "core/models.h"

namespace usb {

// 对整块物理磁盘执行安全弹出。targets 是该磁盘上的卷。
EjectResult EjectDevice(std::uint32_t disk_number, const std::wstring& title,
                        const std::vector<EjectTarget>& targets);

// 单独关闭占用；供提权助手复用。
EjectResult EjectAfterRelease(std::uint32_t disk_number,
                              const std::wstring& title,
                              const std::vector<wchar_t>& letters);

}  // namespace usb
