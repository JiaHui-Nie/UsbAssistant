#include "core/settings.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>

#include "core/log.h"

namespace usb {
namespace {

constexpr wchar_t kSection[] = L"General";
constexpr wchar_t kShortcutName[] = L"U盘助手.lnk";

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) return L".";
    std::wstring text(path, length);
    const std::size_t slash = text.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    text.resize(slash);
    return text;
}

bool DirectoryWritable(const std::wstring& directory) {
    const std::wstring probe = directory + L"\\config.ini";
    HANDLE handle = CreateFileW(probe.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
}

std::wstring AppDataDirectory() {
    PWSTR app_data = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                       &app_data)) &&
        app_data) {
        base = app_data;
        CoTaskMemFree(app_data);
    } else {
        wchar_t fallback[MAX_PATH] = {};
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, fallback) ==
            S_OK) {
            base = fallback;
        }
    }
    if (base.empty()) return L".";
    return base + L"\\UsbAssistant";
}

std::wstring ResolveConfigPath() {
    return DataDirectory() + L"\\config.ini";
}

std::wstring StartupShortcutPath() {
    wchar_t startup[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, startup) != S_OK) {
        return L"";
    }
    return std::wstring(startup) + L"\\" + kShortcutName;
}

int ReadInt(const std::wstring& path, const wchar_t* key, int fallback) {
    return static_cast<int>(
        GetPrivateProfileIntW(kSection, key, fallback, path.c_str()));
}

bool WriteInt(const std::wstring& path, const wchar_t* key, int value) {
    const std::wstring text = std::to_wstring(value);
    return WritePrivateProfileStringW(kSection, key, text.c_str(),
                                      path.c_str()) != FALSE;
}

}  // namespace

std::wstring DataDirectory() {
    const std::wstring portable = ExecutableDirectory();
    if (DirectoryWritable(portable)) return portable;

    const std::wstring app_data = AppDataDirectory();
    if (app_data != L".") CreateDirectoryW(app_data.c_str(), nullptr);
    return app_data;
}

void Settings::Normalize() {
    if (position_mode != 1) position_mode = 0;
    corner = std::clamp(corner, 0, 3);
    refresh_seconds = std::clamp(refresh_seconds, 0, 3600);
    occupants_refresh_seconds =
        std::clamp(occupants_refresh_seconds, 1, 60);
    capacity_base = capacity_base == 1000 ? 1000 : 1024;
    close_action = close_action == 1 ? 1 : 0;
    empty_hide_seconds = std::clamp(empty_hide_seconds, -1, 86400);
    main_height_percent = std::clamp(main_height_percent, 20, 100);
    update_frequency = std::clamp(update_frequency, 0, 4);
    hotkey_mods &= (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    if (hotkey_vk <= 0 || hotkey_vk > 0xFF) hotkey_vk = 0;
    // 全局快捷键必须带 Ctrl/Alt/Win，避免吞掉正常输入；不合法时视为关闭。
    if (hotkey_vk == 0 ||
        (hotkey_mods & (MOD_ALT | MOD_CONTROL | MOD_WIN)) == 0) {
        hotkey_mods = 0;
        hotkey_vk = 0;
    }
}

SettingsStore::SettingsStore() : path_(ResolveConfigPath()) {}

SettingsStore::SettingsStore(std::wstring path) : path_(std::move(path)) {}

Settings SettingsStore::Load() const {
    Settings settings;
    const int config_version = ReadInt(path_, L"ConfigVersion", 0);
    settings.top_most = ReadInt(path_, L"TopMost", 1) != 0;
    settings.position_mode = ReadInt(path_, L"PosMode", 0);
    settings.corner = ReadInt(path_, L"Corner", 3);
    settings.refresh_seconds = ReadInt(path_, L"RefreshSec", 3);
    settings.auto_check_updates =
        ReadInt(path_, L"AutoCheckUpdates", 1) != 0;
    settings.update_frequency = ReadInt(path_, L"UpdateFrequency", 0);
    settings.silent_update = ReadInt(path_, L"SilentUpdate", 0) != 0;
    settings.occupants_refresh_seconds =
        ReadInt(path_, L"OccupantsRefreshSec", 1);
    settings.capacity_base = ReadInt(path_, L"CapacityBase", 1024);
    settings.close_action = ReadInt(path_, L"CloseAction", 1);
    settings.skip_close_confirm =
        ReadInt(path_, L"SkipCloseConfirm", 0) != 0;
    settings.empty_hide_seconds = ReadInt(path_, L"EmptyHideSec", 0);
    settings.edge_collapse = ReadInt(path_, L"EdgeCollapse", 0) != 0;
    settings.hotkey_mods = ReadInt(path_, L"HotkeyMods", 0);
    settings.hotkey_vk = ReadInt(path_, L"HotkeyVk", 0);
    settings.show_hidden_partitions =
        ReadInt(path_, L"ShowHiddenPartitions", 0) != 0;
    settings.show_esp_partitions =
        ReadInt(path_, L"ShowEspPartitions", 1) != 0;
    settings.show_msr_partitions =
        ReadInt(path_, L"ShowMsrPartitions", 1) != 0;
    settings.show_other_hidden_partitions =
        ReadInt(path_, L"ShowOtherHiddenPartitions", 1) != 0;
    settings.main_height_percent = ReadInt(path_, L"MainHeight", 90);
    settings.has_last_position = ReadInt(path_, L"HasLastPos", 0) != 0;
    settings.last_x = ReadInt(path_, L"LastX", 0);
    settings.last_y = ReadInt(path_, L"LastY", 0);
    settings.Normalize();
    settings.auto_start = AutoStartEnabled();
    USB_LOG_INFO(L"config",
                 L"加载配置：%s version=%d top_most=%d pos_mode=%d corner=%d "
                 L"refresh=%d close_action=%d hidden=%d esp=%d msr=%d other=%d "
                 L"edge=%d height=%d occupants_refresh=%d auto_update=%d "
                 L"update_freq=%d silent_update=%d",
                 path_.c_str(), config_version, settings.top_most ? 1 : 0,
                 settings.position_mode, settings.corner,
                 settings.refresh_seconds, settings.close_action,
                 settings.show_hidden_partitions ? 1 : 0,
                 settings.show_esp_partitions ? 1 : 0,
                 settings.show_msr_partitions ? 1 : 0,
                 settings.show_other_hidden_partitions ? 1 : 0,
                 settings.edge_collapse ? 1 : 0,
                 settings.main_height_percent,
                 settings.occupants_refresh_seconds,
                 settings.auto_check_updates ? 1 : 0,
                 settings.update_frequency,
                 settings.silent_update ? 1 : 0);

    // v2 起默认“关闭 = 隐藏到托盘”。旧配置没有版本号，迁移一次，
    // 让已经安装过的用户也能保持托盘常驻；迁移后用户仍可在设置中改回退出。
    if (config_version < 2) {
        settings.close_action = 1;
        Save(settings);
    }
    return settings;
}

bool SettingsStore::Save(const Settings& settings) const {
    bool ok = true;
    ok = WriteInt(path_, L"ConfigVersion", 2) && ok;
    ok = WriteInt(path_, L"TopMost", settings.top_most ? 1 : 0) && ok;
    ok = WriteInt(path_, L"PosMode", settings.position_mode) && ok;
    ok = WriteInt(path_, L"Corner", settings.corner) && ok;
    ok = WriteInt(path_, L"RefreshSec", settings.refresh_seconds) && ok;
    ok = WriteInt(path_, L"AutoCheckUpdates",
                  settings.auto_check_updates ? 1 : 0) &&
         ok;
    ok = WriteInt(path_, L"UpdateFrequency", settings.update_frequency) && ok;
    ok = WriteInt(path_, L"SilentUpdate", settings.silent_update ? 1 : 0) && ok;
    ok = WriteInt(path_, L"OccupantsRefreshSec",
                  settings.occupants_refresh_seconds) &&
         ok;
    ok = WriteInt(path_, L"CapacityBase", settings.capacity_base) && ok;
    ok = WriteInt(path_, L"CloseAction", settings.close_action) && ok;
    ok = WriteInt(path_, L"SkipCloseConfirm",
                  settings.skip_close_confirm ? 1 : 0) &&
         ok;
    ok = WriteInt(path_, L"EmptyHideSec", settings.empty_hide_seconds) && ok;
    ok = WriteInt(path_, L"EdgeCollapse", settings.edge_collapse ? 1 : 0) && ok;
    ok = WriteInt(path_, L"HotkeyMods", settings.hotkey_mods) && ok;
    ok = WriteInt(path_, L"HotkeyVk", settings.hotkey_vk) && ok;
    ok = WriteInt(path_, L"ShowHiddenPartitions",
                  settings.show_hidden_partitions ? 1 : 0) &&
         ok;
    ok = WriteInt(path_, L"ShowEspPartitions",
                  settings.show_esp_partitions ? 1 : 0) &&
         ok;
    ok = WriteInt(path_, L"ShowMsrPartitions",
                  settings.show_msr_partitions ? 1 : 0) &&
         ok;
    ok = WriteInt(path_, L"ShowOtherHiddenPartitions",
                  settings.show_other_hidden_partitions ? 1 : 0) &&
         ok;
    ok = WriteInt(path_, L"MainHeight", settings.main_height_percent) && ok;
    ok = WriteInt(path_, L"HasLastPos", settings.has_last_position ? 1 : 0) && ok;
    ok = WriteInt(path_, L"LastX", settings.last_x) && ok;
    ok = WriteInt(path_, L"LastY", settings.last_y) && ok;
    USB_LOG_INFO(L"config",
                 L"保存配置：%s 结果=%d top_most=%d close_action=%d hidden=%d "
                 L"pos_mode=%d corner=%d",
                 path_.c_str(), ok ? 1 : 0, settings.top_most ? 1 : 0,
                 settings.close_action,
                 settings.show_hidden_partitions ? 1 : 0,
                 settings.position_mode, settings.corner);
    return ok;
}

bool SettingsStore::AutoStartEnabled() {
    const std::wstring path = StartupShortcutPath();
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SettingsStore::SetAutoStart(bool enabled) {
    const std::wstring path = StartupShortcutPath();
    if (path.empty()) return false;

    if (!enabled) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return true;
        }
        return DeleteFileW(path.c_str()) != FALSE;
    }

    wchar_t module[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return false;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(init);

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    bool ok = false;
    if (SUCCEEDED(hr) && link) {
        link->SetPath(module);
        link->SetArguments(L"--autostart");
        link->SetDescription(L"U 盘助手");

        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                           reinterpret_cast<void**>(&file))) &&
            file) {
            ok = SUCCEEDED(file->Save(path.c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }

    if (uninit) CoUninitialize();
    return ok;
}

}  // namespace usb
