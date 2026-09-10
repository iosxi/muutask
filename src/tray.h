#pragma once

#include <windows.h>

#include <string>

#include "media.h"
#include "theme.h"

class App;

// タスクバーの通知領域 (トレイ) のアイコンとメニュー。
class Tray {
public:
    //: 描くアイコンの一辺 (px)。通知領域では 16x16 まで縮められる
    static constexpr int kIconSize = 64;
    //: Windows のツールチップは 127 文字まで
    static constexpr size_t kTooltipLimit = 120;

    ~Tray();

    bool Create(App* app, HWND owner);
    void Close();

    void Update(media::NowPlaying const& state);

    /// トレイからの通知 (uCallbackMessage) を処理する。
    void OnMessage(WPARAM wparam, LPARAM lparam);
    /// エクスプローラーが再起動したとき。アイコンを置き直す。
    void OnTaskbarCreated();

private:
    bool AddIcon();
    HICON MakeIcon() const;
    static std::wstring Tooltip(media::NowPlaying const& state);

    App* app_ = nullptr;
    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
    std::wstring tooltip_;
    theme::Palette palette_{};
};
