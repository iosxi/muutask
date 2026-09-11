#pragma once

#include <windows.h>

#include <optional>
#include <string>

#include "config.h"
#include "media.h"

// バーの右クリックとトレイの右クリックで、同じメニューを出す。
namespace menu {

struct Choice {
    wchar_t const* value;
    wchar_t const* label;
};

struct SizeChoice {
    int value;
    wchar_t const* label;
};

/// タスクバー内での置き場所
inline constexpr Choice kAnchors[] = {
    {L"left", L"左端"},
    {L"start", L"スタート ボタンの左"},
    {L"tray", L"通知領域の左"},
};

/// メニューから選べるバーの幅 (96 DPI 基準の px)
inline constexpr SizeChoice kWidths[] = {
    {240, L"狭い"}, {340, L"標準"}, {460, L"広い"}, {600, L"とても広い"},
};

/// メニューから選べる小窓の大きさ (96 DPI 基準の px)。
/// 正方形でない絵もあるので、この値は「一辺」ではなく「長辺の上限」。
inline constexpr SizeChoice kArtSizes[] = {
    {180, L"標準"}, {300, L"大きい"}, {430, L"とても大きい"}, {560, L"最大"},
};

/// 選ばれた項目。
struct Selection {
    enum class Kind {
        None,
        PlayPause,
        Next,
        Prev,
        Anchor,
        Width,
        ArtSize,
        Session,
        AudioOutput,
        HideWhenIdle,
        WheelVolume,
        Quit,
    };

    Kind kind = Kind::None;
    std::wstring anchor;                      // Kind::Anchor
    int size = 0;                             // Kind::Width / Kind::ArtSize
    std::optional<std::wstring> session;      // Kind::Session (無指定 = 自動)
    std::wstring device;                      // Kind::AudioOutput (デバイスの id)
};

/// メニューを出して、選ばれた項目を返す。
///
/// owner は WS_EX_NOACTIVATE でない普通のウィンドウを渡すこと。バー自身を
/// 渡すとフォアグラウンドになれず、メニューの外を押しても閉じない。
Selection Show(HWND owner, int x, int y, Config const& config,
               media::NowPlaying const& state);

}  // namespace menu
