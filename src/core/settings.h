// 配置模型与持久化。配置文件是 exe 同目录的 config.ini；
// 目录不可写时退回 %APPDATA%\UsbAssistant\config.ini。
#pragma once

#include <string>

namespace usb {

// 配置与日志共用的可写目录：exe 同目录可写则用它，否则 %APPDATA%\UsbAssistant。
std::wstring DataDirectory();

struct Settings {
    bool auto_start = false;        // 由启动文件夹快捷方式决定，不写入 ini
    bool top_most = true;
    int position_mode = 0;          // 0 = 固定角落，1 = 记住上次位置
    int corner = 3;                 // 0 左上 1 左下 2 右上 3 右下
    int refresh_seconds = 3;        // 0 = 关闭定时刷新
    // 自动检查更新的总开关；具体频率由 update_frequency 决定。
    bool auto_check_updates = true;
    // 更新频率：0 软件启动时、1 每小时、2 每天、3 每周、4 每月。
    // 只在软件运行期间自行判断，不注册系统计划任务。
    int update_frequency = 0;
    // 静默更新：发现新版本时不弹窗，直接结束主程序并完成替换后重启。
    bool silent_update = false;
    // 占用列表后台复检间隔（秒）。只写配置文件，不在设置界面展示。
    int occupants_refresh_seconds = 1;
    int capacity_base = 1024;       // 1000 = 厂商进制
    int close_action = 1;           // 0 = 退出，1 = 隐藏到托盘（托盘常驻）
    bool skip_close_confirm = false;
    int empty_hide_seconds = 0;     // -1 = 立即隐藏，0 = 不自动隐藏
    bool edge_collapse = false;
    // 全局快捷键：mods 为 MOD_* 位组合，vk 为虚拟键码；vk == 0 表示未设置。
    int hotkey_mods = 0;
    int hotkey_vk = 0;
    bool show_hidden_partitions = false;
    bool show_esp_partitions = true;
    bool show_msr_partitions = true;
    bool show_other_hidden_partitions = true;
    int main_height_percent = 90;   // 主界面最大高度（可用高度百分比）
    int last_x = 0;
    int last_y = 0;
    bool has_last_position = false;

    void Normalize();
};

class SettingsStore {
public:
    SettingsStore();
    explicit SettingsStore(std::wstring path);

    const std::wstring& path() const { return path_; }
    Settings Load() const;
    bool Save(const Settings& settings) const;

    // 启动文件夹快捷方式（不写注册表）。
    static bool AutoStartEnabled();
    static bool SetAutoStart(bool enabled);

private:
    std::wstring path_;
};

}  // namespace usb
