#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "image.h"
#include "media.h"
#include "win32util.h"

class App;

// タスクバーの空きスペースに重ねて表示する、曲情報 + 操作ボタンのバー。
//
// Windows 11 ではタスクバーへのツール バー追加 (デスク バンド) が廃止されている
// ため、タスクバーとぴったり重なる最前面ウィンドウを置き、位置と重なり順を
// 定期的に追従させることで「タスクバーの中にいる」ように見せている。
class Bar {
public:
    //: 既定のバー幅 (96 DPI 基準の px)
    static constexpr int kDefaultWidth = 340;
    //: 隣の要素との間隔 (96 DPI 基準の px)
    static constexpr int kDefaultGap = 8;
    //: これより狭い場所には置かない (96 DPI 基準の px)
    static constexpr int kMinWidth = 120;

    Bar() = default;
    ~Bar();

    bool Create(App* app, HINSTANCE instance);
    void Close();

    HWND hwnd() const { return hwnd_; }
    bool visible() const { return visible_; }
    /// 画面座標での (x, y, 幅, 高さ)。まだ出ていなければ無し。
    std::optional<RECT> geometry() const;

    /// タスクバーの位置・高さに追従し、重なり順を保つ。
    void Sync();
    void UpdateState(std::shared_ptr<media::NowPlaying const> state);
    void UpdateProgress();
    /// 流れる文字を 1 コマ進める。
    void ScrollTick();
    /// 設定に合わせて、ホイールの横取りを付け外しする。
    void ApplyWheelVolume();
    /// 小窓の大きさが変わったとき。出ていれば新しい大きさで出し直す。
    void RefreshPopup();

private:
    struct Colors {
        Rgb bg, title, artist, button, hover, disabled, track, end, chip, chip_hover;
    };

    struct Layout {
        int width = 0, height = 0;
        int pad = 0, art = 0;
        int text_x = 0, text_width = 0;
        int title_y = 0, artist_y = 0;
        int prog_x0 = 0, prog_x1 = 0, prog_y = 0, end_h = 0;
        int chip_w = 0, chip_h = 0, radius = 0, cy = 0;
        int prev_cx = 0, play_cx = 0, next_cx = 0;
        int title_px = 0, artist_px = 0, icon_px = 0;
    };

    enum class Button { None, Prev, Play, Next };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

    Colors MakeColors(std::optional<Rgb> sampled) const;
    void SampleBackground(int x, int y, int width, int height);
    void Build(int width, int height);
    void EnsureFonts();
    void ReleaseFonts();
    void EnsureSurface(int width, int height);
    void Render();
    void PaintRows(HDC dc, int x0, int x1, int height);
    void DrawButtons(HDC dc);
    void SetText(wchar_t const* key, std::wstring const& text);
    int MeasureText(HFONT font, std::wstring const& text);

    Button HitTest(int x, int y) const;
    bool Enabled(Button button) const;
    void SetHover(Button button);
    void Hide(wchar_t const* reason = nullptr);
    void KeepInFront(int cx, int cy);
    std::optional<std::pair<int, int>> Placement(RECT const& taskbar) const;
    bool WheelTarget(int x, int y) const;
    bool OnWheel(int x, int y, int delta);
    void ArmPopup();
    void CancelPopup();
    void OpenPopup();

    App* app_ = nullptr;
    HWND hwnd_ = nullptr;
    bool visible_ = false;

    std::shared_ptr<media::NowPlaying const> state_ =
        std::make_shared<media::NowPlaying>();

    std::optional<RECT> geometry_;          // 画面座標 (left, top, right, bottom)
    std::optional<std::pair<int, int>> built_size_;
    std::wstring anchor_used_;
    Colors colors_{};
    std::vector<Rgb> bg_rows_;              // タスクバーの行ごとの色
    Layout layout_{};

    // 描画先 (二重描きを避けるためのメモリ DC)
    HDC surface_ = nullptr;
    HBITMAP surface_bitmap_ = nullptr;
    HGDIOBJ surface_old_ = nullptr;
    int surface_w_ = 0, surface_h_ = 0;

    // フォント
    std::wstring text_family_;
    std::wstring icon_family_;
    bool icon_is_mdl2_ = false;
    HFONT f_title_ = nullptr;
    HFONT f_artist_ = nullptr;
    HFONT f_icon_ = nullptr;
    HDC measure_ = nullptr;

    // アルバム アート
    std::wstring art_key_;
    image::Bgra art_image_;

    // ボタンの下に敷く半透明の座 (大きさが変わったときだけ焼き直す)
    image::Bgra chip_normal_;
    image::Bgra chip_hover_;

    // 入力
    Button hover_ = Button::None;
    Button pressed_ = Button::None;
    bool tracking_mouse_ = false;
    std::optional<uint64_t> covered_since_;

    // ホイール
    std::unique_ptr<win32util::WheelHook> wheel_hook_;
    int wheel_accum_ = 0;  // まだ 1 ノッチに満たないホイールの残り

    // 流れる文字 (title / artist の 2 行ぶん)
    struct Line {
        std::wstring text;      // 描くもの (流すときは 2 回並べたもの)
        std::wstring source;    // 元の文字列 (同じかどうかの判定用)
        int period = 0;         // 0 なら流さない (収まっている)
        double offset = 0.0;
        uint64_t pause_until = 0;
    };
    Line title_{};
    Line artist_{};
    uint64_t scrolled_at_ = 0;

    bool popup_armed_ = false;
};
