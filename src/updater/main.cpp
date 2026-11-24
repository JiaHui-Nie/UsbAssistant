// 升级器入口：
//   普通启动     -> 弹出「检查更新」窗口
//   --autocheck  -> 静默检查，只有发现新版本才显示窗口
//   --silent     -> 完全静默：不显示窗口，发现新版本直接替换并重启主程序
//   --cleanup    -> 只清理上一次自我替换留下的 .old，不联网、不显示窗口
//
// 每次启动都会先清理 .old：升级器自我替换时会把正在运行的本体改名成
// UsbAssistantUpdater.old.exe，新版本直接写成正式文件名。
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

#include "app/updater_app.h"
#include "core/app_control.h"
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
    bool silent = false;
    bool cleanup = false;
    bool autocheck = false;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        silent = HasArgument(argc, argv, L"--silent");
        cleanup = HasArgument(argc, argv, L"--cleanup");
        autocheck = HasArgument(argc, argv, L"--autocheck");
        LocalFree(argv);
    }

    usb::log::Init();
    // 无论这次要不要检查更新，都先删掉上次自我替换留下的旧版本。
    usb::DeleteUpdaterLeftover();

    int result = 0;
    if (cleanup) {
        // 只为清理而来：不联网、不建窗口。
        usb::log::Shutdown();
        return 0;
    }

    usb::app::UpdaterApp::UpdateMode mode =
        usb::app::UpdaterApp::UpdateMode::Visible;
    if (silent) {
        mode = usb::app::UpdaterApp::UpdateMode::Silent;
    } else if (autocheck) {
        mode = usb::app::UpdaterApp::UpdateMode::AutoCheck;
    }

    result = 1;
    {
        usb::app::UpdaterApp app;
        switch (app.Initialize(instance, mode)) {
            case usb::app::UpdaterApp::InitResult::Ok:
                result = app.Run();
                break;
            case usb::app::UpdaterApp::InitResult::AlreadyRunning:
                // 请求已转交给现有实例：静默退出，不再新开窗口。
                result = 0;
                break;
            case usb::app::UpdaterApp::InitResult::Failed:
                // 静默更新失败不打扰用户，其余情况给出提示。
                if (!silent) {
                    MessageBoxW(nullptr,
                                L"无法初始化升级器（Direct2D / DirectWrite）。",
                                L"检查更新", MB_ICONERROR | MB_OK);
                }
                break;
        }
    }
    usb::log::Shutdown();
    return result;
}
