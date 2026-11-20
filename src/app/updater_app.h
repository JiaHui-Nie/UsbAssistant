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
    UpdaterApp();
    ~UpdaterApp();
    UpdaterApp(const UpdaterApp&) = delete;
    UpdaterApp& operator=(const UpdaterApp&) = delete;

    bool Initialize(HINSTANCE instance, bool autocheck);
    int Run();

private:
    static LRESULT CALLBACK PumpProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    void StartCheck();
    void HandleCheckResult(int status, bool update_available, bool has_asset,
                           const std::wstring& version,
                           const ReleaseAsset& asset);
    void StartInstall();
    void RunInstall(ReleaseAsset asset);
    void OnWindowClosed();

    void PostInstallStage(int kind, std::wstring text);

    bool autocheck_ = false;
    bool com_initialized_ = false;

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
    bool installing_ = false;
    bool top_most_ = true;
};

}  // namespace usb::app
