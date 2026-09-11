#pragma once

#include <windows.h>

#include <optional>
#include <string>

#include "media.h"
#include "theme.h"
#include "win32util.h"

class App;

// タスクバーの通知領域 (トレイ) のアイコンとメニュー。
class Tray {
public:
    //: 音符アイコンを描く一辺 (px)。通知領域では 16x16 まで縮められる。
    //: 音量の数字はこれを使わず、縮められる前の大きさで直に描く (tray.cpp)
    static constexpr int kIconSize = 64;
    //: Windows のツールチップは 127 文字まで
    static constexpr size_t kTooltipLimit = 120;

    ~Tray();

    bool Create(App* app, HWND owner);
    void Close();

    void Update(media::NowPlaying const& state);

    /// システムの音量。アイコンに出す数字はこれ。読めなければ無しを渡す
    /// (音符のアイコンに戻る)。
    void SetVolume(std::optional<win32util::VolumeMeter::Reading> const& volume);

    /// トレイからの通知 (uCallbackMessage) を処理する。
    void OnMessage(WPARAM wparam, LPARAM lparam);
    /// エクスプローラーが再起動したとき。アイコンを置き直す。
    void OnTaskbarCreated();

private:
    bool AddIcon();
    HICON MakeIcon() const;
    /// アイコンを作り直して置き換える。
    void RefreshIcon();
    /// ツールチップを組み立て直し、変わっていれば差し替える。
    void RefreshTooltip();
    std::wstring Tooltip() const;
    static std::wstring TrackText(media::NowPlaying const& state);

    App* app_ = nullptr;
    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
    std::wstring tooltip_;
    std::wstring track_;
    std::optional<win32util::VolumeMeter::Reading> volume_;
    theme::Palette palette_{};
    //: タスクバーが暗いか。数字を置く色はこちらに背ける
    bool system_dark_ = true;
};
