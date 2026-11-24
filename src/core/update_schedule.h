// 自动检查更新的频率规则与“上次检查时间”的持久化。
// 频率只在软件启动后自行判断：不注册系统计划任务，也不依赖后台服务；
// 软件没运行时不会有任何检查。
#pragma once

#include <cstdint>

namespace usb {

// 与设置界面里的下拉选项一一对应。
enum UpdateFrequency {
    kUpdateOnStartup = 0,  // 每次启动检查一次
    kUpdateHourly = 1,
    kUpdateDaily = 2,
    kUpdateWeekly = 3,
    kUpdateMonthly = 4,  // 固定 30 天
    kUpdateFrequencyCount = 5,
};

// 频率对应的间隔（秒）；启动时模式返回 0，表示不按时间累计。
std::int64_t UpdateIntervalSeconds(int frequency);

// 距上次检查是否已经到期。启动时模式恒为 true（由启动流程单独判断）；
// 没有记录、系统时间被回拨时都不会立刻重试，避免反复打开升级器。
bool UpdateCheckDue(int frequency, std::int64_t last_check, std::int64_t now);

// 当前 Unix 秒数。
std::int64_t NowSeconds();

// 上次检查尝试的时间（Unix 秒）；没有记录时返回 0。
// 时间戳单独放在 config.ini 的 [Update] 段，避免和 Settings 的保存互相覆盖。
std::int64_t ReadLastUpdateCheck();

// 记录一次检查尝试（无论结果如何，避免失败后立刻重试）。
void RecordLastUpdateCheck(std::int64_t now);

}  // namespace usb
