// 升级器入口：
//   普通启动     -> 弹出「检查更新」窗口
//   --autocheck  -> 静默检查，只有发现新版本才显示窗口
//   --silent     -> 完全静默：不显示窗口，发现新版本直接替换并重启主程序
//   --install <包路径> -> 用本地更新包安装，不联网、不比较版本；默认显示进度窗口，
//                        与 --silent 同用时不显示窗口
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

// --install 后面紧跟包路径，路径可能带空格，因此单独取值而不是做前缀匹配。
std::wstring InstallArgument(int argc, wchar_t* const* argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--install") == 0) return argv[i + 1];
    }
    return L"";
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    bool silent = false;
    bool cleanup = false;
    bool autocheck = false;
    bool install_requested = false;
    std::wstring install_package;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        silent = HasArgument(argc, argv, L"--silent");
        cleanup = HasArgument(argc, argv, L"--cleanup");
        autocheck = HasArgument(argc, argv, L"--autocheck");
        install_requested = HasArgument(argc, argv, L"--install");
        install_package = InstallArgument(argc, argv);
        LocalFree(argv);
    }

    usb::log::Init();
    // 无论这次要不要检查更新，都先删掉上次自我替换留下的旧版本。
    usb::DeleteUpdaterLeftover();

    int result = 0;
    if (cleanup && !install_requested) {
        // 只为清理而来：不联网、不建窗口。
        usb::log::Shutdown();
        return 0;
    }

    usb::app::UpdaterApp::UpdateMode mode =
        usb::app::UpdaterApp::UpdateMode::Visible;
    if (install_requested) {
        // 相对路径按当前工作目录解析，避免从快捷方式启动时落到别的目录。
        wchar_t full_path[MAX_PATH] = {};
        const DWORD length =
            install_package.empty()
                ? 0
                : GetFullPathNameW(install_package.c_str(), MAX_PATH,
                                   full_path, nullptr);
        if (length == 0 || length >= MAX_PATH) {
            MessageBoxW(nullptr,
                        install_package.empty()
                            ? L"--install 缺少更新包路径。"
                            : L"更新包路径过长。",
                        L"检查更新", MB_ICONERROR | MB_OK);
            usb::log::Shutdown();
            return 1;
        }
        install_package = full_path;
        const DWORD attributes = GetFileAttributesW(install_package.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            MessageBoxW(nullptr, L"更新包不存在或不是文件。", L"检查更新",
                        MB_ICONERROR | MB_OK);
            USB_LOG_WARN(L"update", L"本地更新包无效：%s",
                         install_package.c_str());
            usb::log::Shutdown();
            return 1;
        }
        mode = silent ? usb::app::UpdaterApp::UpdateMode::LocalInstallSilent
                      : usb::app::UpdaterApp::UpdateMode::LocalInstall;
    } else if (silent) {
        mode = usb::app::UpdaterApp::UpdateMode::Silent;
    } else if (autocheck) {
        mode = usb::app::UpdaterApp::UpdateMode::AutoCheck;
    }

    result = 1;
    {
        usb::app::UpdaterApp app;
        switch (app.Initialize(instance, mode, install_package)) {
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
