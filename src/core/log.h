// 诊断日志：Debug 构建默认开启，Release 默认关闭。
// 关闭时不做任何格式化、不打开文件；可用环境变量 USBASSISTANT_LOG=1 强制开启。
// 日志目录与配置一致：exe 目录可写则同目录，否则 %APPDATA%\UsbAssistant。
#pragma once

#include <string>

namespace usb::log {

enum class Level { Debug, Info, Warn, Error };

// 进程内只初始化一次；重复调用无副作用。
void Init();
// 停止写线程并刷盘；之后的写入调用会被忽略。
void Shutdown();

bool Enabled();

// 运行时开关：开启时若尚未打开文件会按与 Init 相同的规则打开。
void SetEnabled(bool enabled);

// 仅在 Enabled() 为真时调用。
void Write(Level level, const wchar_t* module, const std::wstring& message);

// 调用方保证只在 Enabled() 为真时才会求值参数。
std::wstring Format(const wchar_t* format, ...);

}  // namespace usb::log

#define USB_LOG(level, module, ...)                                        \
    do {                                                                   \
        if (::usb::log::Enabled()) {                                       \
            ::usb::log::Write(level, module,                               \
                              ::usb::log::Format(__VA_ARGS__));            \
        }                                                                  \
    } while (0)

#define USB_LOG_DEBUG(module, ...) \
    USB_LOG(::usb::log::Level::Debug, module, __VA_ARGS__)
#define USB_LOG_INFO(module, ...) \
    USB_LOG(::usb::log::Level::Info, module, __VA_ARGS__)
#define USB_LOG_WARN(module, ...) \
    USB_LOG(::usb::log::Level::Warn, module, __VA_ARGS__)
#define USB_LOG_ERROR(module, ...) \
    USB_LOG(::usb::log::Level::Error, module, __VA_ARGS__)
