#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>

#include "bar.h"
#include "config.h"
#include "media.h"
#include "popup.h"
#include "tray.h"
#include "win32util.h"

/// 起動と各部品の接続、スレッド間の受け渡し。
class App {
public:
    App();
    ~App();

    App(App const&) = delete;
    App& operator=(App const&) = delete;

    bool Initialize(HINSTANCE instance);
    int Run();

    Config& config() { return config_; }
    double scale() const { return scale_; }
    HWND hidden() const { return hidden_; }
    HINSTANCE instance() const { return instance_; }
    media::MediaController& controller() { return *controller_; }
    Popup& popup() { return popup_; }
    std::shared_ptr<media::NowPlaying const> state() const { return state_; }

    // ------------------------------------------------------------------ 操作

    void TogglePlayPause();
    void NextTrack();
    void PreviousTrack();
    void SetAnchor(std::wstring const& anchor);
    void SetWidth(int width);
    void SetArtSize(int height);
    void ToggleHideWhenIdle();
    void ToggleWheelVolume();
    void SelectSession(std::optional<std::wstring> const& app_id);
    void Quit();

    /// バーやトレイからの右クリック。画面座標で受ける。
    void ShowContextMenu(int x, int y);
    /// メニューが開いている間は小窓を出さない (最前面の小窓がメニューに
    /// かぶさってしまうため)。
    bool menu_open() const { return menu_open_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void OnStatePosted();
    /// システムの音量を読み直し、変わっていればトレイのアイコンに映す。
    void SyncVolume();

    HINSTANCE instance_ = nullptr;
    HWND hidden_ = nullptr;
    double scale_ = 1.0;
    bool menu_open_ = false;

    Config config_;
    std::shared_ptr<media::NowPlaying const> state_ =
        std::make_shared<media::NowPlaying>();
    std::unique_ptr<media::MediaController> controller_;
    Bar bar_;
    Popup popup_;
    Tray tray_;
    //: トレイに出す数字の出どころ。COM を使うので、STA のこのスレッドから
    //: だけ触ること
    win32util::VolumeMeter volume_;
};
