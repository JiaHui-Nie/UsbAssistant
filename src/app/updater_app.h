// 升级器控制器：检查更新、下载、解压、替换程序与重启主程序。
// 界面只在 ui::UpdaterWindow 中绘制，这里只做流程编排。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <atomic>
#include <memory>
#include <string>

#include "app/post_target.h"
#include "app/worker.h"
#include "core/update.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/updater_window.h"

namespace usb::app {

class UpdaterApp {
public:
    enum class InitResult {
        Ok,
        AlreadyRunning,  // 已把本次请求转交给现有实例，静默退出
        Failed,
    };

    // 启动方式：
    //   Visible   手动检查，窗口始终显示；
    //   AutoCheck 自动检查，只有发现新版本才显示窗口；
    //   Silent    静默更新，完全不显示窗口，发现新版本直接完成替换。
    enum class UpdateMode { Visible, AutoCheck, Silent };

    UpdaterApp();
    ~UpdaterApp();
    UpdaterApp(const UpdaterApp&) = delete;
    UpdaterApp& operator=(const UpdaterApp&) = delete;

    InitResult Initialize(HINSTANCE instance, UpdateMode mode);
    int Run();

private:
    static LRESULT CALLBACK PumpProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    void StartCheck();
    // 收到重复启动转交：前置窗口，并在空闲时重新发起检查。
    void HandleWakeRequest(bool autocheck);
    // 已有实例存在时，把本次请求转交给它的窗口。
    static void WakeExistingInstance(bool autocheck);
    void HandleCheckResult(int status, bool update_available, bool has_asset,
                           const std::wstring& version,
                           const std::wstring& error,
                           const ReleaseAsset& asset);
    void StartInstall();
    void RunInstall(ReleaseAsset asset);
    void OnWindowClosed();

    void PostInstallStage(int kind, std::wstring text);

    bool IsSilent() const { return mode_ == UpdateMode::Silent; }
    // 静默模式没有窗口：这些封装在无界面时直接忽略。
    void ShowStage(ui::UpdaterStage stage, std::wstring text);
    void ShowProgress(float fraction);
    // 结束本次启动：有窗口时关窗（关窗即退出消息循环），无窗口时直接退出。
    void Finish();

    UpdateMode mode_ = UpdateMode::Visible;
    bool autocheck_ = false;
    bool com_initialized_ = false;
    // 单实例互斥体：句柄存活即代表本进程是唯一升级器实例。
    HANDLE instance_mutex_ = nullptr;

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ui::Fonts fonts_;
    ui::Palette palette_ = ui::MakePalette(false);
    ui::UpdaterWindow window_;

    HWND pump_ = nullptr;
    std::shared_ptr<PostTarget> post_target_ = std::make_shared<PostTarget>();
    Worker worker_;
    std::atomic_bool cancel_{false};

    ReleaseAsset asset_;
    bool checking_ = false;
    bool installing_ = false;
};

}  // namespace usb::app
