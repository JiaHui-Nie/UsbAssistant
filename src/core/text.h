// 与界面无关的字符串 / 格式化工具。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usb::text {

std::wstring Trim(std::wstring value);
std::wstring ToLower(std::wstring value);
bool IEquals(const std::wstring& left, const std::wstring& right);
bool StartsWithInsensitive(const std::wstring& text, const std::wstring& prefix);
bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle);

std::vector<std::wstring> SplitWhitespace(const std::wstring& text);
std::wstring Join(const std::vector<std::wstring>& tokens, std::size_t begin,
                  std::size_t end);

// 去掉 USB / Generic / Mass Storage / 2.0 之类通用词后的硬件名称。
std::wstring CleanPnpName(const std::wstring& value);

// 结合厂商与产品名生成设备标题；两者都为空时返回可读的兜底名称。
std::wstring DeviceTitle(const std::wstring& vendor, const std::wstring& product,
                         bool removable_media);

// 1024 / 1000 两种容量进制。
std::wstring FormatBytes(std::uint64_t bytes, int capacity_base);

bool IsOpenableFileSystem(const std::wstring& filesystem);

// 可执行文件的版本描述（FileDescription），失败返回空串。
std::wstring FileDescription(const std::wstring& path);

// 去掉路径与扩展名的文件名。
std::wstring BaseNameWithoutExtension(const std::wstring& path);

}  // namespace usb::text
