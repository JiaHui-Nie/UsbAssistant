// 程序入口：
//   普通启动 -> 托盘助手（单实例）
//   --autostart -> 静默驻留托盘
//   --helper    -> UAC 提权助手，完成解除占用 + 弹出后退出
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <memory>
#include <string>

#include "app/application.h"
#include "core/elevation.h"
#include "core/log.h"

namespace {

constexpr wchar_t kMutexName[] = L"UsbAssistant_SingleInstance_2026";
constexpr wchar_t kWindowClass[] = L"UsbAssistantMain";

bool HasArgument(int argc, wchar_t* const* argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], name) == 0) return true;
    }
    return false;
}

HWND FindMainWindow() {
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (HWND hwnd = FindWindowW(kWindowClass, nullptr)) return hwnd;
        Sleep(50);
    }
    return nullptr;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;

    usb::HelperRequest helper;
    if (usb::ParseHelperArguments(argc, argv, &helper)) {
        LocalFree(argv);
        usb::log::Init();
        const int result = usb::RunHelper(helper);
        usb::log::Shutdown();
        return result;
    }

    const bool autostart = HasArgument(argc, argv, L"--autostart");
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindMainWindow()) {
            PostMessageW(existing, WM_APP + 7, 0, 0);  // kMessageShowMain
        }
        CloseHandle(mutex);
        return 0;
    }

    auto application = std::make_unique<usb::app::Application>();
    usb::log::Init();
    if (!application->Initialize(instance, autostart)) {
        usb::log::Shutdown();
        MessageBoxW(nullptr,
                    L"初始化失败：无法创建 Direct2D / DirectWrite 环境。",
                    L"U 盘助手", MB_ICONERROR | MB_OK);
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    const int result = application->Run();
    application.reset();
    usb::log::Shutdown();
    if (mutex) CloseHandle(mutex);
    return result;
}
