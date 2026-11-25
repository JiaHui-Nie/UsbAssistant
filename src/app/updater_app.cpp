#include "app/updater_app.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <utility>

#include "core/app_control.h"
#include "core/http.h"
#include "core/log.h"
#include "core/update_schedule.h"
#include "core/version.h"
#include "core/zip.h"
#include "ui/dpi.h"

namespace usb::app {
namespace {

constexpr UINT kMessageCheckDone = WM_APP + 1;
constexpr UINT kMessageDownloadProgress = WM_APP + 2;
constexpr UINT kMessageInstallStage = WM_APP + 3;

constexpr wchar_t kPumpClass[] = L"UsbAssistantUpdaterPump";

// 单实例互斥体名称：Local 命名空间按登录会话隔离，符合桌面程序语义。
constexpr wchar_t kUpdaterMutexName[] =
    L"Local\\UsbAssistant.Updater.SingleInstance";

struct CheckPayload {
    int status = 0;
    bool update_available = false;
    bool has_asset = false;
    std::wstring version;
    std::wstring error;
    ReleaseAsset asset;
};

struct ProgressPayload {
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    bool total_known = false;
};

enum class InstallStage { Updating, Failed, Done };

struct StagePayload {
    InstallStage kind = InstallStage::Updating;
    std::wstring text;
};

template <typename T>
void PostPayload(const std::shared_ptr<PostTarget>& target, UINT message,
                 std::unique_ptr<T> payload) {
    if (!target) return;
    std::lock_guard<std::mutex> lock(target->mutex);
    if (!target->hwnd) return;
    if (!PostMessageW(target->hwnd, message, 0,
                      reinterpret_cast<LPARAM>(payload.get()))) {
        return;
    }
    payload.release();
}

template <typename T>
std::unique_ptr<T> TakePayload(LPARAM lparam) {
    return std::unique_ptr<T>(reinterpret_cast<T*>(lparam));
}

void RemoveTree(const std::wstring& path) {
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring child = path + L"\\" + data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveTree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

std::wstring MakeTempDirectory() {
    wchar_t temp_path[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp_path);
    if (length == 0 || length >= MAX_PATH) return L"";

    std::wstring directory = temp_path;
    directory += L"UsbAssistant-update-";
    directory += std::to_wstring(GetCurrentProcessId());
    RemoveTree(directory);
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return L"";
    return directory;
}

bool CopyWithRetry(const std::wstring& source, const std::wstring& target) {
    // 主程序刚退出时文件可能还被短暂占用，多试几次。
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (CopyFileW(source.c_str(), target.c_str(), FALSE)) return true;
        Sleep(150);
    }
    return false;
}

// 升级器替换自身：正在运行的 exe 不能删除但可以改名，所以先把本体改成
// .old，再把新版本写成正式文件名；.old 留到下一次升级器启动时删除。
bool ReplaceUpdaterSelf(const std::wstring& app_dir,
                        const std::wstring& new_updater) {
    const std::wstring target = app_dir + L"\\" + kUpdaterExeName;
    const std::wstring backup = app_dir + L"\\" + kUpdaterOldExeName;

    // 上一次留下的备份可能因为旧进程未退出而残留，先尽力清掉。
    SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(backup.c_str());

    if (!MoveFileExW(target.c_str(), backup.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        USB_LOG_WARN(L"update", L"升级器自我改名失败：%lu", GetLastError());
        return false;
    }
    if (!CopyWithRetry(new_updater, target)) {
        // 新版本没写成功时恢复原名，避免目录里只剩一个 .old。
        MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        USB_LOG_ERROR(L"update", L"写入新版升级器失败：%lu", GetLastError());
        return false;
    }
    USB_LOG_INFO(L"update", L"升级器已替换为新版本，旧版本留待下次启动清理");
    return true;
}

}  // namespace

UpdaterApp::UpdaterApp() = default;

UpdaterApp::~UpdaterApp() {
    cancel_.store(true);
    worker_.Stop(true);
    {
        std::lock_guard<std::mutex> lock(post_target_->mutex);
        post_target_->hwnd = nullptr;
    }
    if (pump_) {
        DestroyWindow(pump_);
        pump_ = nullptr;
    }
    window_.Close();
    ui::ReleaseFonts(&fonts_);
    if (dwrite_) {
        dwrite_->Release();
        dwrite_ = nullptr;
    }
    if (d2d_) {
        d2d_->Release();
        d2d_ = nullptr;
    }
    if (com_initialized_) CoUninitialize();
    if (instance_mutex_) {
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
    }
}

UpdaterApp::InitResult UpdaterApp::Initialize(HINSTANCE instance,
                                              UpdateMode mode) {
    mode_ = mode;
    autocheck_ = mode != UpdateMode::Visible;

    // 单实例：重复启动不再创建窗口，把请求转交给已有实例后静默退出。
    // 互斥体只负责覆盖“两个进程同时启动”的竞争；具体请求仍通过窗口
    // 消息转交，避免两个进程同时下载/安装更新包。
    // 静默模式没有窗口可转交，重复启动直接退出。
    instance_mutex_ = CreateMutexW(nullptr, FALSE, kUpdaterMutexName);
    const DWORD mutex_error = GetLastError();
    if (!instance_mutex_) {
        if (mutex_error == ERROR_ACCESS_DENIED) {
            // 已有实例权限更高（例如以管理员身份运行），打不开互斥体。
            if (IsSilent()) return InitResult::AlreadyRunning;
            WakeExistingInstance(autocheck_);
            return InitResult::AlreadyRunning;
        }
        USB_LOG_WARN(L"update", L"创建单实例互斥体失败：%lu", mutex_error);
    } else if (mutex_error == ERROR_ALREADY_EXISTS) {
        if (IsSilent()) {
            USB_LOG_INFO(L"update", L"已有升级器在运行，本次静默更新请求退出");
            return InitResult::AlreadyRunning;
        }
        WakeExistingInstance(autocheck_);
        return InitResult::AlreadyRunning;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(com);
    ui::EnableDpiAwareness();

    palette_ = ui::MakePalette(ui::IsSystemDarkMode());
    // 只用于定位：升级器不再读取主程序的置顶状态，窗口始终置顶。
    const RunningApp running = FindRunningApp();

    // 界面资源只在需要显示窗口时创建；静默更新完全不加载 Direct2D。
    if (!IsSilent()) {
        const GUID d2d1_factory_iid = {
            0x06152247, 0x6f50, 0x465a,
            {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     d2d1_factory_iid, nullptr,
                                     reinterpret_cast<void**>(&d2d_))) ||
            !d2d_) {
            return InitResult::Failed;
        }

        const GUID dwrite_factory_iid = {
            0xb859ee5a, 0xd838, 0x4b5b,
            {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       dwrite_factory_iid,
                                       reinterpret_cast<IUnknown**>(&dwrite_))) ||
            !dwrite_) {
            return InitResult::Failed;
        }
        if (!ui::CreateFonts(dwrite_, &fonts_)) return InitResult::Failed;
    }

    static bool pump_registered = false;
    if (!pump_registered) {
        WNDCLASSEXW pump_class = {};
        pump_class.cbSize = sizeof(pump_class);
        pump_class.lpfnWndProc = &UpdaterApp::PumpProc;
        pump_class.hInstance = instance;
        pump_class.lpszClassName = kPumpClass;
        pump_registered = RegisterClassExW(&pump_class) != 0;
        if (!pump_registered) return InitResult::Failed;
    }

    pump_ = CreateWindowExW(0, kPumpClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, instance, this);
    if (!pump_) return InitResult::Failed;
    {
        std::lock_guard<std::mutex> lock(post_target_->mutex);
        post_target_->hwnd = pump_;
    }

    if (!IsSilent()) {
        const HWND reference = running.window;
        if (!window_.Open(instance, reference, !autocheck_, d2d_, dwrite_,
                          &fonts_, palette_, [this]() { StartInstall(); },
                          [this]() { OnWindowClosed(); })) {
            return InitResult::Failed;
        }
        window_.SetWakeHandler(
            [this](bool autocheck) { HandleWakeRequest(autocheck); });
        window_.SetStage(ui::UpdaterStage::Checking, L"正在检查更新中……");
    }

    worker_.Start();
    StartCheck();
    return InitResult::Ok;
}

int UpdaterApp::Run() {
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

void UpdaterApp::OnWindowClosed() { cancel_.store(true); }

void UpdaterApp::StartCheck() {
    checking_ = true;
    USB_LOG_INFO(L"update", L"开始检查更新：current=%s config=%s arch=%s",
                 kAppVersion, kBuildConfigLabel, kBuildArchLabel);
    if (IsDevBuild()) {
        // 开发构建没有数字版本号，既没有可比对的对象，也就不该占用接口。
        USB_LOG_INFO(L"update", L"开发构建（dev）不参与更新检查");
        RecordLastUpdateCheck(NowSeconds());
        auto payload = std::make_unique<CheckPayload>();
        payload->status = static_cast<int>(ReleaseFetchStatus::Ok);
        PostPayload(post_target_, kMessageCheckDone, std::move(payload));
        return;
    }

    const std::shared_ptr<PostTarget> target = post_target_;
    worker_.Post([target]() {
        auto payload = std::make_unique<CheckPayload>();
        LatestRelease release;
        std::wstring error;
        const ReleaseFetchStatus status = FetchLatestRelease(&release, &error);
        // 无论成功还是失败都记下时间：失败后也要等下一个周期再试。
        RecordLastUpdateCheck(NowSeconds());
        payload->status = static_cast<int>(status);
        payload->error = error;
        if (status == ReleaseFetchStatus::Ok) {
            payload->version = release.version;
            // 所有构建统一按数字版本比较：Debug 与 Release 不再分流。
            payload->update_available =
                CompareVersions(kAppVersion, release.version) < 0;
            ReleaseAsset asset;
            if (SelectReleaseAsset(release, kBuildConfigLabel,
                                   kBuildArchLabel, &asset)) {
                payload->has_asset = true;
                payload->asset = std::move(asset);
            } else {
                USB_LOG_WARN(L"update", L"没有匹配 %s-%s 的发布包",
                             kBuildConfigLabel, kBuildArchLabel);
            }
        } else {
            USB_LOG_WARN(L"update", L"检查更新失败：%s", error.c_str());
        }
        PostPayload(target, kMessageCheckDone, std::move(payload));
    });
}

void UpdaterApp::HandleCheckResult(int status, bool update_available,
                                   bool has_asset, const std::wstring& version,
                                   const std::wstring& error,
                                   const ReleaseAsset& asset) {
    checking_ = false;
    if (status != static_cast<int>(ReleaseFetchStatus::Ok)) {
        if (autocheck_) {
            Finish();
            return;
        }
        ShowStage(ui::UpdaterStage::CheckFailed,
                  error.empty() ? L"检查更新失败。"
                                : L"检查更新失败：" + error);
        return;
    }

    if (!update_available) {
        if (autocheck_) {
            Finish();
            return;
        }
        if (IsDevBuild()) {
            // 不要对开发构建谎称“已是最新版本”。
            ShowStage(ui::UpdaterStage::UpToDate,
                      L"当前是开发版本（dev），不参与更新检查。");
            return;
        }
        ShowStage(ui::UpdaterStage::UpToDate,
                  L"已是最新版本(v" + version + L")。");
        return;
    }

    if (!has_asset) {
        if (autocheck_) {
            Finish();
            return;
        }
        ShowStage(
            ui::UpdaterStage::CheckFailed,
            std::wstring(L"未找到匹配当前构建(") + kBuildConfigLabel + L"-" +
                kBuildArchLabel + L")的发布包。");
        return;
    }

    asset_ = asset;
    USB_LOG_INFO(L"update", L"发现新版本 %s，资产 %s", version.c_str(),
                 asset.name.c_str());
    ShowStage(ui::UpdaterStage::Available,
              L"发现新版本(v" + version + L")。");
    if (IsSilent()) {
        // 静默更新：不询问也不显示进度，直接下载、替换并重启。
        USB_LOG_INFO(L"update", L"静默更新：直接开始安装");
        StartInstall();
        return;
    }
    if (autocheck_) window_.Show();
}

void UpdaterApp::HandleWakeRequest(bool autocheck) {
    if (IsSilent()) {
        USB_LOG_INFO(L"update", L"静默更新进行中，忽略重复启动请求");
        return;
    }
    // 已有实例被显式唤起：先显示并前置。即使本次是 --autocheck 请求，
    // 既然要唤醒已有窗口，就按可见模式处理，避免检查完又自动关闭。
    window_.Show();
    autocheck_ = false;
    if (installing_) {
        USB_LOG_INFO(L"update", L"重复启动：正在下载/安装，仅前置窗口");
        return;
    }
    if (checking_) {
        USB_LOG_INFO(L"update", L"重复启动：正在检查，仅前置窗口");
        return;
    }
    USB_LOG_INFO(L"update", L"重复启动：由现有实例重新检查（autocheck=%d）",
                 autocheck ? 1 : 0);
    asset_ = ReleaseAsset{};
    window_.SetProgress(-1.0f);
    window_.SetStage(ui::UpdaterStage::Checking, L"正在检查更新中……");
    StartCheck();
}

void UpdaterApp::WakeExistingInstance(bool autocheck) {
    const UINT message = RegisterWindowMessageW(ui::kUpdaterWakeMessageName);
    if (message == 0) {
        USB_LOG_WARN(L"update", L"注册升级器唤醒消息失败");
        return;
    }
    const WPARAM wparam =
        autocheck ? ui::kUpdaterWakeAutocheck : ui::kUpdaterWakeVisible;
    // 已有实例可能仍在创建窗口，短暂重试后再放弃。
    for (int attempt = 0; attempt < 40; ++attempt) {
        const HWND window = FindWindowW(ui::kUpdaterWindowClass, nullptr);
        if (window && PostMessageW(window, message, wparam, 0)) {
            USB_LOG_INFO(L"update", L"重复启动请求已转交给现有升级器");
            return;
        }
        Sleep(25);
    }
    USB_LOG_WARN(L"update", L"检测到现有升级器，但请求转交失败");
}

void UpdaterApp::StartInstall() {
    if (installing_) return;
    installing_ = true;
    cancel_.store(false);
    ShowStage(ui::UpdaterStage::Downloading, L"正在下载更新……");
    ShowProgress(-1.0f);
    const ReleaseAsset asset = asset_;
    worker_.Post([this, asset]() { RunInstall(asset); });
}

void UpdaterApp::RunInstall(ReleaseAsset asset) {
    const std::wstring temp_root = MakeTempDirectory();
    if (temp_root.empty()) {
        PostInstallStage(static_cast<int>(InstallStage::Failed),
                         L"更新失败：无法创建临时目录。");
        return;
    }
    const std::wstring zip_path = temp_root + L"\\update.zip";
    const std::wstring extract_dir = temp_root + L"\\extract";

    std::wstring error;
    const bool downloaded = http::Download(
        asset.download_url, zip_path, 30000,
        [this](const http::Progress& progress) {
            auto payload = std::make_unique<ProgressPayload>();
            payload->received = progress.received;
            payload->total = progress.total;
            payload->total_known = progress.total_known;
            PostPayload(post_target_, kMessageDownloadProgress,
                        std::move(payload));
        },
        &cancel_, &error);
    if (!downloaded) {
        RemoveTree(temp_root);
        if (cancel_.load()) return;
        PostInstallStage(static_cast<int>(InstallStage::Failed),
                         L"更新失败：" + error + L"。");
        return;
    }
    if (cancel_.load()) {
        RemoveTree(temp_root);
        return;
    }

    if (!zip::ExtractAll(zip_path, extract_dir, &error)) {
        USB_LOG_WARN(L"update", L"解压失败：%s", error.c_str());
        RemoveTree(temp_root);
        PostInstallStage(static_cast<int>(InstallStage::Failed),
                         L"更新失败：" + error + L"。");
        return;
    }

    const std::wstring app_dir = ExecutableDirectory();
    const std::wstring new_main = extract_dir + L"\\" + kMainExeName;
    const std::wstring new_updater = extract_dir + L"\\" + kUpdaterExeName;
    if (GetFileAttributesW(new_main.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_updater.c_str()) == INVALID_FILE_ATTRIBUTES) {
        RemoveTree(temp_root);
        PostInstallStage(static_cast<int>(InstallStage::Failed),
                         L"更新失败：更新包内容不完整。");
        return;
    }

    // 先结束主程序：优先优雅退出，超时再强制结束。
    const RunningApp running = FindRunningApp();
    const bool was_running = running.window != nullptr;
    // 主窗口不可见说明用户此前把它收进了托盘（或折叠到屏幕边缘）。
    // 重启时要带上 --autostart 才能保持隐藏，否则新进程会把窗口弹出来。
    const bool was_hidden = was_running && !IsWindowVisible(running.window);
    const wchar_t* relaunch_arguments = was_hidden ? L"--autostart" : L"";
    USB_LOG_INFO(L"update", L"更新前主程序正在运行：%d 可见：%d",
                 was_running ? 1 : 0, was_hidden ? 0 : 1);
    if (was_running) {
        const StopAppResult stop = RequestAppExit(running, 5000, &error);
        if (stop == StopAppResult::Failed) {
            RemoveTree(temp_root);
            PostInstallStage(static_cast<int>(InstallStage::Failed),
                             L"更新失败：" + error + L"。");
            return;
        }
    }

    PostInstallStage(static_cast<int>(InstallStage::Updating),
                     L"正在更新……");

    const std::wstring target_main = app_dir + L"\\" + kMainExeName;
    if (!CopyWithRetry(new_main, target_main)) {
        USB_LOG_ERROR(L"update", L"替换主程序失败");
        if (was_running) LaunchApp(target_main, relaunch_arguments, app_dir);
        RemoveTree(temp_root);
        PostInstallStage(static_cast<int>(InstallStage::Failed),
                         L"更新失败：无法替换程序文件。");
        return;
    }

    // 升级器替换自身：本体改名成 .old，新版本写成正式文件名，
    // .old 由下一次升级器启动时删除，主程序不再参与。
    const bool updater_updated = ReplaceUpdaterSelf(app_dir, new_updater);

    RemoveTree(temp_root);

    if (was_running && !LaunchApp(target_main, relaunch_arguments, app_dir)) {
        PostInstallStage(
            static_cast<int>(InstallStage::Failed),
            L"更新已完成，但无法重新启动程序，请手动打开。");
        return;
    }

    if (!updater_updated) {
        PostInstallStage(
            static_cast<int>(InstallStage::Failed),
            L"更新已完成，但升级器更新失败，请重新下载应用程序。");
        return;
    }

    PostInstallStage(static_cast<int>(InstallStage::Done), L"");
}

void UpdaterApp::ShowStage(ui::UpdaterStage stage, std::wstring text) {
    if (IsSilent()) return;
    window_.SetStage(stage, std::move(text));
}

void UpdaterApp::ShowProgress(float fraction) {
    if (IsSilent()) return;
    window_.SetProgress(fraction);
}

void UpdaterApp::Finish() {
    if (IsSilent()) {
        // 没有窗口可关，直接结束消息循环让进程退出。
        PostQuitMessage(0);
        return;
    }
    window_.Close();
}

void UpdaterApp::PostInstallStage(int kind, std::wstring text) {
    auto payload = std::make_unique<StagePayload>();
    payload->kind = static_cast<InstallStage>(kind);
    payload->text = std::move(text);
    PostPayload(post_target_, kMessageInstallStage, std::move(payload));
}

LRESULT CALLBACK UpdaterApp::PumpProc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
    UpdaterApp* self = reinterpret_cast<UpdaterApp*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<UpdaterApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT UpdaterApp::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    (void)wparam;
    switch (message) {
        case kMessageCheckDone: {
            auto payload = TakePayload<CheckPayload>(lparam);
            if (!payload) return 0;
            HandleCheckResult(payload->status, payload->update_available,
                              payload->has_asset, payload->version,
                              payload->error, payload->asset);
            return 0;
        }
        case kMessageDownloadProgress: {
            auto payload = TakePayload<ProgressPayload>(lparam);
            if (!payload) return 0;
            const float fraction =
                (payload->total_known && payload->total > 0)
                    ? static_cast<float>(
                          static_cast<double>(payload->received) /
                          static_cast<double>(payload->total))
                    : -1.0f;
            ShowProgress(fraction);
            return 0;
        }
        case kMessageInstallStage: {
            auto payload = TakePayload<StagePayload>(lparam);
            if (!payload) return 0;
            switch (payload->kind) {
                case InstallStage::Updating:
                    ShowStage(ui::UpdaterStage::Updating, L"正在更新……");
                    break;
                case InstallStage::Failed:
                    // 失败后允许重复启动请求重新检查/下载。
                    installing_ = false;
                    ShowStage(ui::UpdaterStage::Failed, payload->text);
                    if (IsSilent()) PostQuitMessage(0);
                    break;
                case InstallStage::Done:
                    if (IsSilent()) {
                        PostQuitMessage(0);
                    } else {
                        window_.CloseLater(700);
                    }
                    break;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::app
