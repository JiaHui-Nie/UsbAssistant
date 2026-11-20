// 升级器入口：
//   普通启动     -> 弹出「检查更新」窗口
//   --autocheck  -> 静默检查，只有发现新版本才显示窗口
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

#include "app/updater_app.h"
#include "core/log.h"

namespace {

bool HasArgument(int argc, wchar_t* const* argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], name) == 0) return true;
    }
    return false;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    bool autocheck = false;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        autocheck = HasArgument(argc, argv, L"--autocheck");
        LocalFree(argv);
    }

    usb::log::Init();
    int result = 1;
    {
        usb::app::UpdaterApp app;
        if (app.Initialize(instance, autocheck)) {
            result = app.Run();
        } else {
            MessageBoxW(nullptr,
                        L"无法初始化升级器（Direct2D / DirectWrite）。",
                        L"检查更新", MB_ICONERROR | MB_OK);
        }
    }
    usb::log::Shutdown();
    return result;
}
