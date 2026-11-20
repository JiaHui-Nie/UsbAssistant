// 核心逻辑测试：不创建窗口，可在 CI / 无设备环境运行。
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/devices.h"
#include "core/eject.h"
#include "core/elevation.h"
#include "core/log.h"
#include "core/occupants.h"
#include "core/settings.h"
#include "core/storage.h"
#include "core/text.h"
#include "core/update.h"
#include "core/version.h"
#include "core/zip.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__,              \
                        #condition);                                         \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        const std::wstring actual_value = (actual);                          \
        const std::wstring expected_value = (expected);                      \
        if (actual_value != expected_value) {                                \
            std::printf("FAIL %s:%d  expected [%ls] got [%ls]\n", __FILE__,  \
                        __LINE__, expected_value.c_str(),                    \
                        actual_value.c_str());                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

std::wstring TempConfigPath() {
    wchar_t temp[MAX_PATH] = {};
    wchar_t file[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"ucf", 0, file);
    return file;
}

void TestText() {
    using namespace usb::text;
    CHECK_EQ(Trim(L"  hello \t\r\n"), L"hello");
    CHECK_EQ(ToLower(L"AbC"), L"abc");
    CHECK(IEquals(L"abc", L"ABC"));
    CHECK(StartsWithInsensitive(L"USB2.0", L"usb"));
    CHECK(ContainsInsensitive(L"SanDisk Cruzer", L"cruzer"));

    CHECK_EQ(CleanPnpName(L"USB Mass Storage Device"), L"");
    CHECK_EQ(CleanPnpName(L"SanDisk Cruzer Blade USB Device"),
             L"SanDisk Cruzer Blade");
    CHECK_EQ(CleanPnpName(L"Generic Flash Disk USB Device"), L"Flash Disk");
    CHECK_EQ(DeviceTitle(L"USB", L"Mass Storage", true), L"可移动磁盘");
    CHECK_EQ(DeviceTitle(L"SanDisk", L"Cruzer Blade", true),
             L"SanDisk Cruzer Blade");
    CHECK_EQ(DeviceTitle(L"", L"Portable SSD T7", false), L"Portable SSD T7");

    CHECK_EQ(FormatBytes(0, 1024), L"0 B");
    CHECK_EQ(FormatBytes(1024, 1024), L"1.00 KB");
    CHECK_EQ(FormatBytes(1000, 1000), L"1.00 KB");
    CHECK_EQ(FormatBytes(1024ull * 1024 * 1024, 1024), L"1.00 GB");

    CHECK(IsOpenableFileSystem(L"NTFS"));
    CHECK(IsOpenableFileSystem(L"exfat"));
    CHECK(!IsOpenableFileSystem(L"未知文件系统"));
    CHECK_EQ(BaseNameWithoutExtension(L"C:\\Tools\\demo.exe"), L"demo");
}

void TestSettings() {
    CHECK(usb::Settings().close_action == 1);  // 默认关闭 = 隐藏到托盘
    CHECK(usb::Settings().auto_check_updates);
    CHECK_EQ(std::to_wstring(usb::Settings().occupants_refresh_seconds), L"1");
    CHECK(usb::Settings().show_esp_partitions);
    CHECK(usb::Settings().show_msr_partitions);
    CHECK(usb::Settings().show_other_hidden_partitions);

    usb::Settings settings;
    settings.position_mode = 9;
    settings.corner = 17;
    settings.capacity_base = 2048;
    settings.main_height_percent = 5;
    settings.close_action = 3;
    settings.refresh_seconds = -4;
    settings.occupants_refresh_seconds = 0;
    settings.Normalize();
    CHECK_EQ(std::to_wstring(settings.occupants_refresh_seconds), L"1");
    CHECK_EQ(std::to_wstring(settings.position_mode), L"0");
    CHECK_EQ(std::to_wstring(settings.corner), L"3");
    CHECK_EQ(std::to_wstring(settings.capacity_base), L"1024");
    CHECK_EQ(std::to_wstring(settings.main_height_percent), L"20");
    CHECK_EQ(std::to_wstring(settings.close_action), L"0");
    CHECK_EQ(std::to_wstring(settings.refresh_seconds), L"0");

    const std::wstring path = TempConfigPath();
    settings = usb::Settings();
    settings.top_most = false;
    settings.corner = 1;
    settings.refresh_seconds = 8;
    settings.auto_check_updates = false;
    settings.capacity_base = 1000;
    settings.close_action = 1;
    settings.skip_close_confirm = true;
    settings.edge_collapse = true;
    settings.empty_hide_seconds = 30;
    settings.show_hidden_partitions = true;
    settings.show_esp_partitions = false;
    settings.show_msr_partitions = false;
    settings.show_other_hidden_partitions = true;
    settings.main_height_percent = 55;
    settings.occupants_refresh_seconds = 5;
    settings.has_last_position = true;
    settings.last_x = -120;
    settings.last_y = 240;

    {
        usb::SettingsStore store(path);
        CHECK(store.Save(settings));
    }
    {
        usb::SettingsStore store(path);
        const usb::Settings loaded = store.Load();
        CHECK(loaded.top_most == false);
        CHECK_EQ(std::to_wstring(loaded.corner), L"1");
        CHECK_EQ(std::to_wstring(loaded.refresh_seconds), L"8");
        CHECK(!loaded.auto_check_updates);
        CHECK_EQ(std::to_wstring(loaded.capacity_base), L"1000");
        CHECK_EQ(std::to_wstring(loaded.close_action), L"1");
        CHECK(loaded.skip_close_confirm);
        CHECK(loaded.edge_collapse);
        CHECK_EQ(std::to_wstring(loaded.empty_hide_seconds), L"30");
        CHECK(loaded.show_hidden_partitions);
        CHECK(!loaded.show_esp_partitions);
        CHECK(!loaded.show_msr_partitions);
        CHECK(loaded.show_other_hidden_partitions);
        CHECK_EQ(std::to_wstring(loaded.main_height_percent), L"55");
        CHECK_EQ(std::to_wstring(loaded.occupants_refresh_seconds), L"5");
        CHECK(loaded.has_last_position);
        CHECK_EQ(std::to_wstring(loaded.last_x), L"-120");
        CHECK_EQ(std::to_wstring(loaded.last_y), L"240");
    }
    DeleteFileW(path.c_str());

    // 旧配置（无 ConfigVersion）迁移为“关闭=隐藏”；迁移后用户选择退出仍被尊重。
    const std::wstring migration_path = TempConfigPath();
    WritePrivateProfileStringW(L"General", L"CloseAction", L"0",
                               migration_path.c_str());
    {
        usb::SettingsStore store(migration_path);
        const usb::Settings legacy = store.Load();
        CHECK(legacy.close_action == 1);
        // 旧配置没有 OccupantsRefreshSec，必须按默认 1 秒加载。
        CHECK_EQ(std::to_wstring(legacy.occupants_refresh_seconds), L"1");
        // 旧配置没有 AutoCheckUpdates，按默认开启加载。
        CHECK(legacy.auto_check_updates);
        CHECK(GetPrivateProfileIntW(L"General", L"ConfigVersion", 0,
                                    migration_path.c_str()) == 2);
        WritePrivateProfileStringW(L"General", L"CloseAction", L"0",
                                   migration_path.c_str());
        CHECK(store.Load().close_action == 0);
    }
    DeleteFileW(migration_path.c_str());
}

void TestStorage() {
    CHECK_EQ(usb::storage::OpenableVolumePath(L"\\\\?\\Volume{1}\\"),
             L"\\\\?\\Volume{1}");
    CHECK_EQ(usb::storage::VolumeDisplayName(L'F'), L"F:");
    CHECK_EQ(usb::storage::VolumeDisplayName(0), L"该分区");

    usb::EjectTarget target;
    target.letter = L'Z';
    // 盘符 Z: 通常不存在，除非用户恰好挂载了它。
    const bool present = usb::storage::AnyTargetLetterPresent({target});
    const DWORD drives = GetLogicalDrives();
    const bool expected = (drives & (1u << ('z' - 'a'))) != 0;
    CHECK(present == expected);
}

void TestHelperArguments() {
    usb::HelperRequest request;
    request.disk_number = 7;
    request.letters = {L'F', L'G'};
    request.result_path = L"C:\\Temp\\usb-result.txt";

    const std::wstring command_line =
        L"UsbAssistant.exe " + usb::BuildHelperArguments(request);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command_line.c_str(), &argc);
    CHECK(argv != nullptr);

    usb::HelperRequest parsed;
    CHECK(usb::ParseHelperArguments(argc, argv, &parsed));
    CHECK_EQ(std::to_wstring(parsed.disk_number), L"7");
    CHECK(parsed.letters == request.letters);
    CHECK_EQ(parsed.result_path, request.result_path);
    if (argv) LocalFree(argv);

    // 空盘符列表也必须能正确往返，且不能吞掉后面的 --result。
    usb::HelperRequest empty_request;
    empty_request.disk_number = 3;
    empty_request.result_path = L"C:\\Temp\\empty-result.txt";
    const std::wstring empty_line =
        L"UsbAssistant.exe " + usb::BuildHelperArguments(empty_request);
    int empty_argc = 0;
    wchar_t** empty_argv = CommandLineToArgvW(empty_line.c_str(), &empty_argc);
    CHECK(empty_argv != nullptr);
    usb::HelperRequest empty_parsed;
    CHECK(usb::ParseHelperArguments(empty_argc, empty_argv, &empty_parsed));
    CHECK(empty_parsed.letters.empty());
    CHECK_EQ(empty_parsed.result_path, empty_request.result_path);
    if (empty_argv) LocalFree(empty_argv);

    usb::HelperRequest mount_request;
    mount_request.operation = usb::HelperOperation::Mount;
    mount_request.volume_path = L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\";
    mount_request.result_path = L"C:\\Temp\\mount-result.txt";
    const std::wstring mount_line =
        L"UsbAssistant.exe " + usb::BuildHelperArguments(mount_request);
    int mount_argc = 0;
    wchar_t** mount_argv = CommandLineToArgvW(mount_line.c_str(), &mount_argc);
    CHECK(mount_argv != nullptr);
    usb::HelperRequest mount_parsed;
    CHECK(usb::ParseHelperArguments(mount_argc, mount_argv, &mount_parsed));
    CHECK(mount_parsed.operation == usb::HelperOperation::Mount);
    CHECK_EQ(mount_parsed.volume_path, mount_request.volume_path);
    CHECK_EQ(mount_parsed.result_path, mount_request.result_path);
    if (mount_argv) LocalFree(mount_argv);

    usb::HelperRequest unmount_request;
    unmount_request.operation = usb::HelperOperation::Unmount;
    unmount_request.letter = L'F';
    unmount_request.result_path = L"C:\\Temp\\unmount-result.txt";
    const std::wstring unmount_line =
        L"UsbAssistant.exe " + usb::BuildHelperArguments(unmount_request);
    int unmount_argc = 0;
    wchar_t** unmount_argv =
        CommandLineToArgvW(unmount_line.c_str(), &unmount_argc);
    CHECK(unmount_argv != nullptr);
    usb::HelperRequest unmount_parsed;
    CHECK(usb::ParseHelperArguments(unmount_argc, unmount_argv,
                                    &unmount_parsed));
    CHECK(unmount_parsed.operation == usb::HelperOperation::Unmount);
    CHECK(unmount_parsed.letter == L'F');
    CHECK_EQ(unmount_parsed.result_path, unmount_request.result_path);
    if (unmount_argv) LocalFree(unmount_argv);

    usb::HelperRequest open_request;
    open_request.operation = usb::HelperOperation::Open;
    open_request.root = L"F:\\";
    open_request.result_path = L"C:\\Temp\\open-result.txt";
    const std::wstring open_line =
        L"UsbAssistant.exe " + usb::BuildHelperArguments(open_request);
    int open_argc = 0;
    wchar_t** open_argv = CommandLineToArgvW(open_line.c_str(), &open_argc);
    CHECK(open_argv != nullptr);
    usb::HelperRequest open_parsed;
    CHECK(usb::ParseHelperArguments(open_argc, open_argv, &open_parsed));
    CHECK(open_parsed.operation == usb::HelperOperation::Open);
    CHECK_EQ(open_parsed.root, open_request.root);
    CHECK_EQ(open_parsed.result_path, open_request.result_path);
    if (open_argv) LocalFree(open_argv);
}

void TestDevicesSmoke() {
    const auto devices = usb::EnumerateDevices();
    for (const auto& device : devices) {
        CHECK(!device.title.empty());
        CHECK(!device.partitions.empty());
        CHECK(device.total > 0);
        CHECK(device.has_letter() || device.partitions.front().letter == 0);
    }
}

void TestEjectSafety() {
    // 不存在的磁盘号 + 空目标：必须快速返回 NotFound，而不是尝试任何 IOCTL。
    const usb::EjectResult result = usb::EjectDevice(9999, L"", {});
    CHECK(result.status == usb::EjectStatus::NotFound);
    CHECK(!result.message.empty());

    CHECK(usb::FindOccupants({}).empty());
    CHECK(usb::storage::WaitDiskGone(9999, {}, 0));

    usb::EjectTarget target;
    target.letter = 0;
    CHECK(!usb::storage::AnyTargetLetterPresent({target}));
}

void TestLogFormat() {
    // 未调用 Init 时日志必须处于关闭状态（测试不产生日志文件）。
    CHECK(!usb::log::Enabled());
    CHECK_EQ(usb::log::Format(L"%s=%d", L"disk", 7), L"disk=7");
    CHECK_EQ(usb::log::Format(L"error=0x%08lX",
                              static_cast<unsigned long>(0x2A)),
             L"error=0x0000002A");
    CHECK_EQ(usb::log::Format(L"%s", L"\\\\?\\Volume{0}\\\\"),
             L"\\\\?\\Volume{0}\\\\");
}

void TestVersionAndUpdate() {
    using namespace usb;
    CHECK_EQ(NormalizeVersion(L" v1.2.3 "), L"1.2.3");
    CHECK_EQ(NormalizeVersion(L"V2.0"), L"2.0");
    CHECK_EQ(NormalizeVersion(L"1.2.1"), L"1.2.1");

    CHECK(CompareVersions(L"1.2.3", L"v1.2.3") == 0);
    CHECK(CompareVersions(L"1.2.3", L"1.2.4") < 0);
    CHECK(CompareVersions(L"1.10.0", L"1.9.9") > 0);
    CHECK(CompareVersions(L"2.0", L"2.0.0") == 0);
    CHECK(CompareVersions(L"1.2.3", L"1.2.3-beta") == 0);
    CHECK(CompareVersions(L"abc", L"1.2.1") == 0);  // 无法比较时按相等处理

    const std::string json =
        R"({"tag_name":"v1.4.0","html_url":)"
        R"("https://github.com/JiaHui-Nie/UsbAssistant/releases/tag/v1.4.0"})";
    std::wstring tag;
    std::wstring url;
    std::wstring error;
    CHECK(ParseLatestRelease(json, &tag, &url, &error));
    CHECK_EQ(tag, L"v1.4.0");
    CHECK_EQ(url,
             L"https://github.com/JiaHui-Nie/UsbAssistant/releases/tag/v1.4.0");

    CHECK(!ParseLatestRelease(R"({"message":"Not Found"})", &tag, &url,
                              &error));
    CHECK(!ParseLatestRelease(
        R"({"tag_name":"","html_url":"https://github.com/x/y"})", &tag, &url,
        &error));
    CHECK(!ParseLatestRelease(
        R"({"tag_name":"v1.2.1","html_url":"file:///etc/passwd"})", &tag,
        &url, &error));
    CHECK(!ParseLatestRelease(R"({"tag_name":"v1.2.1"})", &tag, &url, &error));

    CHECK(!std::wstring(kAppVersion).empty());
#if defined(USBASSISTANT_DEBUG_BUILD) || defined(USBASSISTANT_DEV_BUILD)
    // 开发版：关于界面统一只显示 dev。
    CHECK_EQ(std::wstring(kAboutVersion), L"dev");
#else
    // 正式版：关于界面只显示纯数字版本号。
    const std::wstring about_version = kAboutVersion;
    CHECK(about_version.find(L'v') == std::wstring::npos);
    CHECK(about_version.find(L'-') == std::wstring::npos);
#endif
}

void TestReleaseAssets() {
    using namespace usb;
    const std::string json =
        R"({"tag_name":"v1.2.1","name":"UsbAssistant v1.2.1",)"
        R"("html_url":"https://github.com/JiaHui-Nie/UsbAssistant/releases/tag/v1.2.1",)"
        R"("assets":[)"
        R"({"name":"UsbAssistant-v1.2.1-debug-x64.zip","browser_download_url":)"
        R"("https://github.com/x/y/releases/download/v1.2.1/UsbAssistant-v1.2.1-debug-x64.zip"},)"
        R"({"name":"UsbAssistant-v1.2.1-release-x64.zip","browser_download_url":)"
        R"("https://github.com/x/y/releases/download/v1.2.1/UsbAssistant-v1.2.1-release-x64.zip"},)"
        R"({"name":"UsbAssistant-v1.2.1-debug-x86.zip","browser_download_url":)"
        R"("https://github.com/x/y/releases/download/v1.2.1/UsbAssistant-v1.2.1-debug-x86.zip"},)"
        R"({"name":"UsbAssistant-v1.2.1-release-x86.zip","browser_download_url":)"
        R"("https://github.com/x/y/releases/download/v1.2.1/UsbAssistant-v1.2.1-release-x86.zip"}]})";

    std::vector<ReleaseAsset> assets;
    CHECK(ParseReleaseAssets(json, &assets));
    CHECK(assets.size() == 4);

    LatestRelease release;
    release.assets = assets;
    ReleaseAsset picked;
    // 四种组合必须各取各的，不能拿错维度。
    CHECK(SelectReleaseAsset(release, L"debug", L"x64", &picked));
    CHECK_EQ(picked.name, L"UsbAssistant-v1.2.1-debug-x64.zip");
    CHECK(SelectReleaseAsset(release, L"release", L"x64", &picked));
    CHECK_EQ(picked.name, L"UsbAssistant-v1.2.1-release-x64.zip");
    CHECK(SelectReleaseAsset(release, L"debug", L"x86", &picked));
    CHECK_EQ(picked.name, L"UsbAssistant-v1.2.1-debug-x86.zip");
    CHECK(SelectReleaseAsset(release, L"release", L"x86", &picked));
    CHECK_EQ(picked.name, L"UsbAssistant-v1.2.1-release-x86.zip");
    CHECK(!SelectReleaseAsset(release, L"release", L"arm64", &picked));
    CHECK_EQ(ReleaseAssetSuffix(L"debug", L"x86"), L"-debug-x86.zip");

    CHECK(!ParseReleaseAssets(R"({"tag_name":"v1.0.0"})", &assets));
}

// 由 Python zipfile 以 DEFLATE 生成的真实压缩包，覆盖 inflate 与 CRC 校验。
const unsigned char kSampleZip[] = {
    0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0xef, 0xa6,
    0x74, 0x5d, 0x9b, 0x96, 0xf9, 0xb2, 0xd0, 0x00, 0x00, 0x00, 0x63, 0x0c,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x55, 0x73, 0x62, 0x41, 0x73, 0x73,
    0x69, 0x73, 0x74, 0x61, 0x6e, 0x74, 0x2e, 0x65, 0x78, 0x65, 0x9d, 0xd6,
    0xbb, 0x0d, 0xc2, 0x40, 0x10, 0x45, 0xd1, 0x9c, 0x2a, 0xa6, 0x04, 0xcf,
    0xcf, 0x9f, 0x90, 0x22, 0x5c, 0xc0, 0x22, 0x6f, 0x60, 0xc9, 0x32, 0x16,
    0xbb, 0x04, 0x74, 0x4f, 0x4c, 0x86, 0x6e, 0x01, 0x37, 0x3b, 0x9a, 0x79,
    0x6b, 0x7b, 0xdc, 0x5b, 0xdb, 0x5b, 0x2f, 0x67, 0x97, 0xf7, 0xb5, 0x95,
    0x5e, 0x5f, 0xd2, 0x6b, 0xeb, 0x72, 0x95, 0xcf, 0xf1, 0x2c, 0x9b, 0x1c,
    0xfb, 0x59, 0x45, 0x65, 0x50, 0xf3, 0xc8, 0x71, 0x9a, 0x97, 0xdb, 0xfa,
    0x5f, 0x63, 0xa0, 0x71, 0xd0, 0x04, 0x68, 0x12, 0x34, 0x23, 0x68, 0x26,
    0xd0, 0xcc, 0xa0, 0x59, 0x40, 0xa3, 0x03, 0x89, 0x88, 0x04, 0x25, 0x14,
    0x94, 0x58, 0x50, 0x82, 0x41, 0x89, 0x06, 0x25, 0x1c, 0x94, 0x78, 0x50,
    0x02, 0x42, 0x89, 0x08, 0x23, 0x22, 0x0c, 0xdd, 0x06, 0x22, 0xc2, 0x88,
    0x08, 0x23, 0x22, 0x8c, 0x88, 0x30, 0x22, 0xc2, 0x88, 0x08, 0x23, 0x22,
    0x8c, 0x88, 0x70, 0x22, 0xc2, 0x89, 0x08, 0x47, 0xef, 0x82, 0x88, 0x70,
    0x22, 0xc2, 0x89, 0x08, 0x27, 0x22, 0x9c, 0x88, 0x70, 0x22, 0xc2, 0x89,
    0x88, 0x20, 0x22, 0x82, 0x88, 0x08, 0x22, 0x22, 0xd0, 0x82, 0x20, 0x22,
    0x82, 0x88, 0x08, 0x22, 0x22, 0x88, 0x88, 0x20, 0x22, 0x82, 0x88, 0x48,
    0x22, 0x22, 0x89, 0x88, 0x24, 0x22, 0x92, 0x88, 0x48, 0x34, 0x2a, 0x89,
    0x88, 0x24, 0x22, 0x92, 0x88, 0x48, 0x22, 0x22, 0x89, 0x88, 0xf1, 0x47,
    0xc4, 0x17, 0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00,
    0xef, 0xa6, 0x74, 0x5d, 0x43, 0x3d, 0xa7, 0x66, 0xd2, 0x00, 0x00, 0x00,
    0x65, 0x0c, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x55, 0x73, 0x62, 0x41,
    0x73, 0x73, 0x69, 0x73, 0x74, 0x61, 0x6e, 0x74, 0x55, 0x70, 0x64, 0x61,
    0x74, 0x65, 0x72, 0x2e, 0x65, 0x78, 0x65, 0x9d, 0xd6, 0xbb, 0x0d, 0x83,
    0x30, 0x14, 0x46, 0xe1, 0x3e, 0x53, 0x78, 0x04, 0xee, 0xcb, 0x40, 0x99,
    0x0d, 0xd2, 0x30, 0x80, 0x23, 0x5c, 0x20, 0x21, 0x82, 0x62, 0xa7, 0xc8,
    0xf6, 0xa9, 0xd3, 0xa1, 0x33, 0xc0, 0xe9, 0x3e, 0xdd, 0xfb, 0x2f, 0xed,
    0x79, 0x6f, 0x6d, 0x6b, 0xbd, 0x1c, 0x3d, 0x7d, 0xce, 0xb5, 0xf4, 0xfa,
    0x4e, 0xbd, 0xb6, 0x9e, 0xce, 0xf2, 0xdd, 0x5f, 0x65, 0x4d, 0xfb, 0x76,
    0xd4, 0x24, 0x69, 0x10, 0x35, 0x8f, 0x3c, 0x4e, 0xf3, 0x6d, 0xb9, 0xd6,
    0x28, 0x68, 0x0c, 0x34, 0x0e, 0x9a, 0x00, 0x4d, 0x06, 0xcd, 0x08, 0x9a,
    0x09, 0x34, 0x33, 0x68, 0x64, 0x20, 0x11, 0x91, 0x20, 0x84, 0x82, 0x10,
    0x0b, 0x42, 0x30, 0x08, 0xd1, 0x20, 0x84, 0x83, 0x10, 0x0f, 0x42, 0x40,
    0x08, 0x11, 0xa1, 0x44, 0x84, 0xa2, 0xdb, 0x40, 0x44, 0x28, 0x11, 0xa1,
    0x44, 0x84, 0x12, 0x11, 0x4a, 0x44, 0x28, 0x11, 0xa1, 0x44, 0x84, 0x12,
    0x11, 0x46, 0x44, 0x18, 0x11, 0x61, 0xe8, 0x5d, 0x10, 0x11, 0x46, 0x44,
    0x18, 0x11, 0x61, 0x44, 0x84, 0x11, 0x11, 0x46, 0x44, 0x18, 0x11, 0xe1,
    0x44, 0x84, 0x13, 0x11, 0x4e, 0x44, 0x38, 0x5a, 0x10, 0x44, 0x84, 0x13,
    0x11, 0x4e, 0x44, 0x38, 0x11, 0xe1, 0x44, 0x84, 0x13, 0x11, 0x41, 0x44,
    0x04, 0x11, 0x11, 0x44, 0x44, 0x10, 0x11, 0x81, 0x46, 0x25, 0x11, 0x11,
    0x44, 0x44, 0x10, 0x11, 0x41, 0x44, 0x04, 0x11, 0x91, 0xff, 0x45, 0x3c,
    0x7e, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x08,
    0x00, 0xef, 0xa6, 0x74, 0x5d, 0x9b, 0x96, 0xf9, 0xb2, 0xd0, 0x00, 0x00,
    0x00, 0x63, 0x0c, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x55,
    0x73, 0x62, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x61, 0x6e, 0x74, 0x2e,
    0x65, 0x78, 0x65, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00,
    0x00, 0x08, 0x00, 0xef, 0xa6, 0x74, 0x5d, 0x43, 0x3d, 0xa7, 0x66, 0xd2,
    0x00, 0x00, 0x00, 0x65, 0x0c, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0xfe, 0x00, 0x00,
    0x00, 0x55, 0x73, 0x62, 0x41, 0x73, 0x73, 0x69, 0x73, 0x74, 0x61, 0x6e,
    0x74, 0x55, 0x70, 0x64, 0x61, 0x74, 0x65, 0x72, 0x2e, 0x65, 0x78, 0x65,
    0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
    0x83, 0x00, 0x00, 0x00, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00,
};

std::string ExpectedZipPayload() {
    std::string text;
    for (int line = 1; line <= 60; ++line) {
        text += "UsbAssistant updater test payload line " +
                std::to_string(line) + " 0123456789\n";
    }
    return text;
}

bool ReadFileBytes(const std::filesystem::path& path, std::string* out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out->assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    return true;
}

void TestZipExtract() {
    namespace fs = std::filesystem;
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0) return;

    const fs::path root =
        fs::path(temp) / (L"usbassistant-zip-" +
                          std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root, ignored);

    const fs::path zip_path = root / L"sample.zip";
    {
        std::ofstream output(zip_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(kSampleZip),
                     sizeof(kSampleZip));
    }

    const fs::path extract_dir = root / L"extract";
    std::wstring error;
    CHECK(usb::zip::ExtractAll(zip_path.wstring(), extract_dir.wstring(),
                               &error));
    CHECK(error.empty());

    const std::string expected = ExpectedZipPayload();
    std::string main_bytes;
    std::string updater_bytes;
    CHECK(ReadFileBytes(extract_dir / L"UsbAssistant.exe", &main_bytes));
    CHECK(ReadFileBytes(extract_dir / L"UsbAssistantUpdater.exe",
                        &updater_bytes));
    CHECK(main_bytes == expected);
    CHECK(updater_bytes == expected + "UP");

    // 截断的包必须明确失败。
    const fs::path broken_path = root / L"broken.zip";
    {
        std::ofstream output(broken_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(kSampleZip), 100);
    }
    error.clear();
    CHECK(!usb::zip::ExtractAll(broken_path.wstring(),
                                (root / L"broken").wstring(), &error));
    CHECK(!error.empty());

    fs::remove_all(root, ignored);
}

void TestOccupantLiveness() {
    // Occupant 只可移动，测试里用 vector 承载。
    std::vector<usb::Occupant> alive;
    usb::Occupant self;
    self.pid = GetCurrentProcessId();
    alive.push_back(std::move(self));
    CHECK(usb::OccupantsStillAlive(alive));
    CHECK(usb::OccupantsStillAlive(std::vector<usb::Occupant>()));

    std::vector<usb::Occupant> missing;
    usb::Occupant gone;
    gone.pid = 0xFFFFFFF0u;
    missing.push_back(std::move(gone));
    CHECK(!usb::OccupantsStillAlive(missing));
}

}  // namespace

int main() {
    TestText();
    TestSettings();
    TestStorage();
    TestHelperArguments();
    TestLogFormat();
    TestVersionAndUpdate();
    TestReleaseAssets();
    TestZipExtract();
    TestOccupantLiveness();
    TestDevicesSmoke();
    TestEjectSafety();

    if (g_failures == 0) {
        std::printf("all core tests passed\n");
        return 0;
    }
    std::printf("%d core test(s) failed\n", g_failures);
    return 1;
}
