// 最小 ZIP 解压：支持 STORED / DEFLATE，校验 CRC，拒绝越界路径。
// 只依赖 Win32，不引入外置压缩库。
#pragma once

#include <string>

namespace usb::zip {

// 把 zip 内的全部文件解压到 dest_dir（目录不存在时自动创建）。
// 失败时返回 false，并在 error 中给出可直接展示的原因。
bool ExtractAll(const std::wstring& zip_path, const std::wstring& dest_dir,
                std::wstring* error);

}  // namespace usb::zip
