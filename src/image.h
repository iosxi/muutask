#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "common.h"

// Pillow でやっていた画素の下ごしらえを自前で行う。
//
// 使うのは WIC (デコードだけ) と、あとは全部この中の計算。もらえるアートは
// 大きくても 150x150 なので、素直に書いても十分に速い。
namespace image {

/// 8bit x 3ch の RGB 画像。詰めて並べる (行の余白なし)。
struct Rgb888 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // r, g, b, r, g, b, ...

    bool empty() const { return width <= 0 || height <= 0; }
    uint8_t* at(int x, int y) { return pixels.data() + ((size_t)y * width + x) * 3; }
    uint8_t const* at(int x, int y) const {
        return pixels.data() + ((size_t)y * width + x) * 3;
    }
};

/// AlphaBlend に渡せる、乗算済みアルファの BGRA 画像。
struct Bgra {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // b, g, r, a

    bool empty() const { return width <= 0 || height <= 0; }
};

/// 0..255 の被覆率。角丸や音符の形を表すのに使う。
using Mask = std::vector<uint8_t>;

// --------------------------------------------------------------- 読み込み

/// JPEG / PNG / BMP などを RGB に展開する。読めなければ無し。
std::optional<Rgb888> Decode(std::vector<uint8_t> const& data);

/// 画像本体を展開せずに大きさだけ読む (画素数の比較用)。
std::optional<SIZE> DecodeSize(std::vector<uint8_t> const& data);

// --------------------------------------------------------------- 加工

/// 切り出す。範囲は呼び出し側で正しくしておくこと。
Rgb888 Crop(Rgb888 const& src, int left, int top, int width, int height);

/// 中央を正方形に切り出す。
Rgb888 CenterSquare(Rgb888 const& src);

/// 縁の帯を落とす。
///
/// YouTube Music は 4:3 のジャケットを正方形のキャンバスに置いて渡してくる
/// ことがある (実測では (18,18,18) の帯)。帯を含めたまま扱うと、小窓の中で
/// ジャケットが小さくなり、上下に地色でない黒が残る。角の色と同じで平らな列を
/// 外側から落とす。絵そのものを食べないよう片側 30% で打ち切る。
Rgb888 TrimPadding(Rgb888 const& src);

/// Lanczos (support 3) で拡大・縮小する。Pillow の LANCZOS と同じ流儀で、
/// 縮小するときはフィルターの方を伸ばして折り返しを防ぐ。
Rgb888 Resize(Rgb888 const& src, int width, int height);

/// 色ノイズをならす。引き伸ばす前の、小さいうちにかける。
///
/// JPEG は色を間引いて畳むので、届くアートの平らな面には緑や紫の斑点が残って
/// いる。引き伸ばすとそれが粒になって浮く。色 (Cb/Cr) だけをぼかし、輝度 (Y)
/// には触らないので、輪郭の鋭さは落ちない。
void BlurChroma(Rgb888& image, double sigma);

/// 引き伸ばした画像の輪郭を締め直す (アンシャープ マスク)。
///
/// 輝度だけを締める。RGB のまま締めると、せっかくならした色の縁が立ち直り、
/// 輪郭に色が付いて見える。
void SharpenLuma(Rgb888& image, double radius, int percent, int threshold);

/// 画素値の平均差 (0..255)。大きさが同じ画像どうしを見比べるのに使う。
double MeanAbsDiff(Rgb888 const& a, Rgb888 const& b);

// --------------------------------------------------------------- 形

/// 4 倍の大きさで GDI に描かせてから縮めて、なめらかな被覆率を得る。
/// draw(dc, ss) は白いブラシで塗るだけでよい (下地は黒)。
Mask RasterizeSupersampled(int width, int height,
                           std::function<void(HDC, int)> const& draw);

/// 角丸の四角い被覆率。
Mask RoundedRectMask(int width, int height, int radius);

// --------------------------------------------------------------- 出力

/// RGB + 被覆率を、乗算済みアルファの BGRA にまとめる。
Bgra Premultiply(Rgb888 const& rgb, Mask const& alpha);

/// 単色 + 被覆率から作る (音符アイコンなど)。
Bgra Premultiply(Rgb color, Mask const& alpha, int width, int height);

/// 上に重ねる。dst は乗算済みアルファ、src も乗算済みアルファ。
void CompositeOver(Bgra& dst, Bgra const& src);

/// 明るさを落とす (一時停止中の表現)。乗算済みなので RGB を縮めるだけでよい。
void Dim(Bgra& image, double amount);

/// メモリ DC に転送できる 32bpp DIB を作る。返るビットマップは呼び出し側が消す。
HBITMAP CreateDib(Bgra const& image);

/// 乗算済みアルファのまま dc の (x, y) へ重ねる。
void AlphaBlit(HDC dc, int x, int y, Bgra const& image);

}  // namespace image
