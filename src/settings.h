#pragma once

#include <windows.h>

// 应用设置：持久化在程序目录的 config.ini（不可写时退回 %APPDATA%）
struct Settings {
    bool autoStart = false;        // 开机自启动（开机启动文件夹里的快捷方式）
    bool topMost = true;           // 窗口始终置顶
    int posMode = 0;               // 0 = 相对角落，1 = 记住上次位置
    int corner = 3;                // 0左上 1左下 2右上 3右下
    int refreshSec = 3;            // 数据轮询间隔（秒），0 = 关闭轮询
    int capacityBase = 1024;       // 1024 = Windows 风格，1000 = 硬盘厂商风格
    int closeAction = 0;           // 0 = 退出程序，1 = 隐藏窗口
    bool skipCloseConfirm = false; // 关闭时不再提示
    int emptyHideSec = 0;          // 无设备时自动隐藏（秒），0 = 不隐藏
    bool edgeCollapse = false;     // 拖到屏幕边缘时收起到悬浮条
    int mainHeightPercent = 90;    // 主界面最大高度占屏幕可用高度的百分比
    int lastX = 0;
    int lastY = 0;
    bool hasLastPos = false;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);
void ApplyAutoStart(bool enable);

// 全局容量进制（供格式化容量的模块读取）
int CapacityBase();
void SetCapacityBase(int base);
