#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 自绘托盘菜单：与主界面同风格（圆角卡片 + 深浅色主题）。
// 返回被点击的项索引，未选择返回 -1。dangerIndex 指定需要用红色显示的项（-1 表示无）。
int ShowTrayMenuPopup(HINSTANCE hinst, POINT screenPoint,
                      const std::vector<std::wstring>& items, int dangerIndex);
