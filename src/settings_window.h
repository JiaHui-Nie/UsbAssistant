#pragma once

#include <windows.h>

#include <functional>

#include "settings.h"

// 打开（或激活）设置窗口。onApply 在任何设置改变后调用，用于即时生效。
void ShowSettingsWindow(HINSTANCE hinst, Settings* settings,
                        std::function<void()> onApply);
