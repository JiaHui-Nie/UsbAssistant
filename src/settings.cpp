#include "settings.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <string>

namespace {

const wchar_t* kSection = L"General";
const wchar_t* kShortcutName = L"U盘助手.lnk";

// 设置文件位置：优先程序目录（便携），不可写时退回 %APPDATA%
std::wstring ConfigPath() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"config.ini";

    // 尝试写入以判断是否可写
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        cached = path;
        return cached;
    }

    wchar_t appData[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData) == S_OK) {
        std::wstring dir = std::wstring(appData) + L"\\UsbAssistant";
        CreateDirectoryW(dir.c_str(), nullptr);
        cached = dir + L"\\config.ini";
    } else {
        cached = path;
    }
    return cached;
}

int ReadInt(const wchar_t* name, int fallback) {
    return static_cast<int>(
        GetPrivateProfileIntW(kSection, name, fallback, ConfigPath().c_str()));
}

void WriteInt(const wchar_t* name, int value) {
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%d", value);
    WritePrivateProfileStringW(kSection, name, buffer, ConfigPath().c_str());
}

// 开机启动文件夹里的快捷方式路径
std::wstring StartupShortcutPath() {
    wchar_t startup[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, startup) != S_OK) {
        return L"";
    }
    return std::wstring(startup) + L"\\" + kShortcutName;
}

} // namespace

static int g_capacityBase = 1024;
int CapacityBase() { return g_capacityBase; }
void SetCapacityBase(int base) { g_capacityBase = (base == 1000) ? 1000 : 1024; }

Settings LoadSettings() {
    Settings s;

    s.topMost = ReadInt(L"TopMost", 1) != 0;
    s.posMode = ReadInt(L"PosMode", 0);
    s.corner = ReadInt(L"Corner", 3);
    s.refreshSec = ReadInt(L"RefreshSec", 3);
    s.capacityBase = ReadInt(L"CapacityBase", 1024);
    s.closeAction = ReadInt(L"CloseAction", 0);
    s.skipCloseConfirm = ReadInt(L"SkipCloseConfirm", 0) != 0;
    s.emptyHideSec = ReadInt(L"EmptyHideSec", 0);
    s.edgeCollapse = ReadInt(L"EdgeCollapse", 0) != 0;
    s.mainHeightPercent = ReadInt(L"MainHeight", 90);
    s.hasLastPos = ReadInt(L"HasLastPos", 0) != 0;
    s.lastX = ReadInt(L"LastX", 0);
    s.lastY = ReadInt(L"LastY", 0);

    if (s.posMode != 1) s.posMode = 0;
    if (s.corner < 0 || s.corner > 3) s.corner = 3;
    if (s.capacityBase != 1000) s.capacityBase = 1024;
    if (s.closeAction != 1) s.closeAction = 0;
    if (s.mainHeightPercent < 20) s.mainHeightPercent = 20;
    if (s.mainHeightPercent > 100) s.mainHeightPercent = 100;
    SetCapacityBase(s.capacityBase);

    std::wstring shortcut = StartupShortcutPath();
    s.autoStart = !shortcut.empty() &&
                  GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES;
    return s;
}

void SaveSettings(const Settings& s) {
    WriteInt(L"TopMost", s.topMost ? 1 : 0);
    WriteInt(L"PosMode", s.posMode);
    WriteInt(L"Corner", s.corner);
    WriteInt(L"RefreshSec", s.refreshSec);
    WriteInt(L"CapacityBase", s.capacityBase);
    WriteInt(L"CloseAction", s.closeAction);
    WriteInt(L"SkipCloseConfirm", s.skipCloseConfirm ? 1 : 0);
    WriteInt(L"EmptyHideSec", s.emptyHideSec);
    WriteInt(L"EdgeCollapse", s.edgeCollapse ? 1 : 0);
    WriteInt(L"MainHeight", s.mainHeightPercent);
    WriteInt(L"HasLastPos", s.hasLastPos ? 1 : 0);
    WriteInt(L"LastX", s.lastX);
    WriteInt(L"LastY", s.lastY);
}

// 用开机启动文件夹的快捷方式实现自启动，不写注册表
void ApplyAutoStart(bool enable) {
    std::wstring path = StartupShortcutPath();
    if (path.empty()) return;

    if (!enable) {
        DeleteFileW(path.c_str());
        return;
    }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link))) ||
        !link) {
        return;
    }
    link->SetPath(exe);
    link->SetArguments(L"--autostart");
    link->SetDescription(L"U 盘助手");

    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                       reinterpret_cast<void**>(&file))) &&
        file) {
        file->Save(path.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}
