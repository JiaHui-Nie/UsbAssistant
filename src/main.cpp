#include "app.h"

#include <cwchar>

// 开机自启时命令行带 --autostart，此时不显示主窗口
static bool HasAutostartFlag() {
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return false;
    return wcsstr(cmd, L"--autostart") != nullptr;
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int) {
    // 单实例：已有一个进程在运行时，激活它的窗口后直接退出
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"UsbAssistant_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = nullptr;
        for (int attempt = 0; attempt < 20 && !existing; ++attempt) {
            existing = FindWindowW(L"UsbAssistantWindow", nullptr);
            if (!existing) Sleep(50);
        }
        if (existing) PostMessageW(existing, WM_APP_SHOW_MAIN, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    App app;
    if (!app.Init(hinst, HasAutostartFlag())) {
        MessageBoxW(nullptr, L"初始化失败：无法创建 Direct2D / DirectWrite 环境。",
                    L"U 盘助手", MB_ICONERROR | MB_OK);
        return 1;
    }
    int code = app.Run();
    if (mutex) CloseHandle(mutex);
    return code;
}
