#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "image.h"

// アルバム アートと、代わりに出す音符アイコンを描く。
namespace artwork {

/// 絵柄を見比べるときの一辺 (px)。大きさの違いを均すために縮めてから比べる。
constexpr int kPrintSize = 16;

/// アルバム アート。取得できなければ音符アイコンを返す。
///
/// もらえる絵は正方形とは限らない。YouTube の動画では 150x83 (16:9) が
/// そのまま来る (実測)。keep_aspect を立てると縦横の比を保ったまま、長辺が
/// size に収まる大きさで返す。立てないときは中央を正方形に切り出す
/// (バーの小さな枠のように、正方形しか置けない場所用)。
image::Bgra AlbumArt(std::vector<uint8_t> const& data, int size, int radius, Rgb fg,
                     Rgb bg, bool keep_aspect);

/// 音符アイコン。bg を渡すと角丸の下地を敷く。
image::Bgra NoteIcon(int size, Rgb fg, std::optional<Rgb> bg, int radius);

/// 数字だけのアイコン。トレイに音量 (0-100) を出すのに使う。
///
/// 通知領域では 16x16 まで縮められる。3 桁を読めるようにするため、下地は
/// 敷かず (bg を渡さなければ透ける)、字は枠いっぱいまで引き伸ばす。桁数が
/// 変わっても大きさが揃うよう、幅と高さの両方で収まる方に合わせる。
image::Bgra NumberIcon(int size, std::wstring const& text, Rgb fg,
                       std::optional<Rgb> bg, int radius);

/// 絵柄の指紋。大きさが違っても、同じ絵なら近い値になる。
std::optional<image::Rgb888> Fingerprint(std::vector<uint8_t> const& data);

/// 2 つの指紋が同じ絵か。片方でも読めなければ「違う」とする。
bool SamePicture(std::optional<image::Rgb888> const& a,
                 std::optional<image::Rgb888> const& b);

}  // namespace artwork
