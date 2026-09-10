#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "image.h"
#include "media.h"
#include "theme.h"

class App;

// バーにマウスを乗せたときに、アルバム アートを大きく出す小窓。
//
// タスクバーのボタンにカーソルを載せると出るプレビューと同じ位置・同じくらいの
// 大きさに合わせている。Windows 11 のプレビューは XAML の中で描かれていて
// ウィンドウとして掴めない (クラス名で辿れるのは 1x1 の ThumbnailDeviceHelperWnd
// だけだった) ため、大きさは実測ではなく既定値を持ち、config.json の
// popup_height で変えられるようにしている。
class Popup {
public:
    //: 小窓の長辺の上限 (96 DPI 基準の px)
    static constexpr int kDefaultHeight = 180;
    //: 内側の余白 (96 DPI 基準の px)
    static constexpr int kPad = 10;
    //: タスクバーとの間隔 (96 DPI 基準の px)
    static constexpr int kGap = 8;
    //: カーソルを乗せてから出るまで (ms)。Windows のプレビューに合わせる
    static constexpr UINT kDelay = 450;

    ~Popup();

    bool Create(App* app, HINSTANCE instance);
    void Close();

    bool visible() const { return visible_; }

    /// バーの真上に出す。bar_rect は画面座標。
    void Show(media::NowPlaying const& state, RECT const& bar_rect);
    void Hide();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

    /// 小窓に収める長辺の上限。
    ///
    /// アートは正方形とは限らない (YouTube の動画では 16:9 が来る) ので、
    /// popup_height は「一辺」ではなく「長辺の上限」として扱う。こうすると
    /// 設定した大きさを超えずに、絵の全体が入る。
    int Limit() const;

    App* app_ = nullptr;
    HWND hwnd_ = nullptr;
    bool visible_ = false;

    theme::Palette palette_{};
    std::wstring key_;
    int box_ = 0;                 // 長辺の上限 (DPI をかけたあとの px)
    SIZE size_{0, 0};             // いまの小窓の大きさ
    SIZE rounded_{0, 0};          // 角を丸めたときの大きさ
    image::Bgra art_{};
    int art_x_ = 0, art_y_ = 0;
};
