#pragma once

#include <windows.h>

#include "common.h"

// Windows のテーマ設定 (ダーク / ライト・アクセント カラー) と DPI まわりの小道具。
// 読み取りだけはレジストリを使う。書き換えは一切しない。
namespace theme {

/// 小窓とトレイ アイコンで使う配色。Python 版の Palette のうち、実際に
/// 参照されている 4 つだけを持つ。
struct Palette {
    bool dark = true;
    Rgb accent;  // トレイの音符の色
    Rgb card;    // 小窓の地色・トレイ アイコンの下地
    Rgb muted;   // アートが無いときの音符
    Rgb track;   // その下地

    static Palette Current();
};

bool IsDarkMode();
Rgb AccentColor();

/// 高 DPI でぼやけないようにする。ウィンドウ作成前に呼ぶこと。
void EnableDpiAwareness();

/// 96 DPI を 1.0 とした表示倍率。
double UiScale();

/// 枠なしウィンドウの角を丸くする (SetWindowRgn)。
void RoundWindowCorners(HWND hwnd, int width, int height, int radius);

}  // namespace theme
