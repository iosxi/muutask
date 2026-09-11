#include "artwork.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace artwork {
namespace {

// この倍率より大きく引き伸ばすときは、色ノイズをならして輪郭を締め直す。
// Chrome 経由のアートは 150x150 までしか来ないので、大きな小窓では必ず通る
constexpr double kUpscaleSharpenFrom = 1.15;
// 締め直しの半径。元の 1 画素ぶんの半分あたりに置くと、輪郭だけに効く
constexpr double kSharpenRadiusRatio = 0.5;
// 半径の下限と上限 (px)。効かなすぎ・輪郭が浮きすぎるのを防ぐ
constexpr double kSharpenRadiusLow = 0.8;
constexpr double kSharpenRadiusHigh = 3.0;
// 締め直しの強さ (%) と、無視する差 (0-255)
constexpr int kSharpenPercent = 110;
constexpr int kSharpenThreshold = 2;
// 色ノイズをならす半径 (元画像の画素で数える) と、その下限・上限
constexpr double kChromaBlurRatio = 0.3;
constexpr double kChromaBlurLow = 0.5;
constexpr double kChromaBlurHigh = 1.2;

// 数字アイコンで、枠のうち字に使う割合 (幅・高さ)。通知領域では 16x16 まで
// 縮められるので、余白はぎりぎりまで削る。
constexpr double kNumberFillWidth = 0.96;
constexpr double kNumberFillHeight = 0.82;

/// 数字用のフォント。細いと 16x16 で潰れるので少し太らせる。
///
/// 等幅の数字 (lfPitchAndFamily に FIXED_PITCH) は指定しない。Segoe UI の
/// プロポーショナル数字の方が 1 の幅が詰まり、3 桁でも横に収まるため。
HFONT MakeDigitFont(int pixels) {
    return CreateFontW(-pixels, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       // 4 倍で描いてから縮めるので、GDI 側の平滑化は要らない。
                       // ClearType のままだと色の付いた縁が被覆率に混ざる。
                       NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

/// 長辺が box に収まる大きさ。縦横の比はそのまま。
SIZE Fit(int width, int height, int box) {
    if (width >= height) {
        int h = RoundToInt((double)box * height / width);
        return SIZE{box, h < 1 ? 1 : h};
    }
    int w = RoundToInt((double)box * width / height);
    return SIZE{w < 1 ? 1 : w, box};
}

}  // namespace

image::Bgra NoteIcon(int size, Rgb fg, std::optional<Rgb> bg, int radius) {
    image::Bgra out;
    out.width = size;
    out.height = size;
    out.pixels.assign((size_t)size * size * 4, 0);

    if (bg) {
        image::Mask const base = image::RoundedRectMask(size, size, radius);
        out = image::Premultiply(*bg, base, size, size);
    }

    // 100 を一辺とした比で置いた形。Python 版の座標をそのまま持ってきている。
    auto note = image::RasterizeSupersampled(size, size, [size](HDC dc, int ss) {
        double const scale = (double)size * ss;
        auto px = [scale](double value) { return (int)(value / 100.0 * scale); };
        // 符頭 2 つ (GDI は右下を含まないので +1 する)
        Ellipse(dc, px(16), px(60), px(48) + 1, px(85) + 1);
        Ellipse(dc, px(56), px(50), px(88) + 1, px(75) + 1);
        // 符幹
        Rectangle(dc, px(42), px(20), px(48) + 1, px(73) + 1);
        Rectangle(dc, px(82), px(10), px(88) + 1, px(63) + 1);
        // 連桁
        POINT beam[4] = {{px(42), px(20)}, {px(88), px(10)}, {px(88), px(26)},
                         {px(42), px(36)}};
        Polygon(dc, beam, 4);
    });

    image::Bgra glyph = image::Premultiply(fg, note, size, size);
    image::CompositeOver(out, glyph);
    return out;
}

image::Bgra NumberIcon(int size, std::wstring const& text, Rgb fg,
                       std::optional<Rgb> bg, int radius) {
    image::Bgra out;
    out.width = size;
    out.height = size;
    out.pixels.assign((size_t)size * size * 4, 0);

    if (bg) {
        image::Mask const base = image::RoundedRectMask(size, size, radius);
        out = image::Premultiply(*bg, base, size, size);
    }
    if (text.empty()) return out;

    auto digits = image::RasterizeSupersampled(size, size, [&text, size](HDC dc, int ss) {
        int const box = size * ss;
        double const room_w = box * kNumberFillWidth;
        double const room_h = box * kNumberFillHeight;

        // 試しの大きさで一度測り、収まる倍率を出してから作り直す。数字には
        // 下に飛び出す部分が無いので、字面の高さは tmAscent から
        // tmInternalLeading (字の上の余白) を引いたものとして見る。
        auto measure = [&](HFONT font, SIZE* extent, TEXTMETRICW* metrics) {
            HGDIOBJ old = SelectObject(dc, font);
            GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), extent);
            GetTextMetricsW(dc, metrics);
            SelectObject(dc, old);
        };

        int const probe_px = box / 2;
        HFONT probe = MakeDigitFont(probe_px);
        if (!probe) return;
        SIZE probe_size{};
        TEXTMETRICW probe_metrics{};
        measure(probe, &probe_size, &probe_metrics);
        DeleteObject(probe);

        int const probe_ink = probe_metrics.tmAscent - probe_metrics.tmInternalLeading;
        if (probe_size.cx <= 0 || probe_ink <= 0) return;
        double const factor = (std::min)(room_w / probe_size.cx, room_h / probe_ink);
        int const px = (std::max)(1, RoundToInt(probe_px * factor));

        HFONT font = MakeDigitFont(px);
        if (!font) return;
        SIZE extent{};
        TEXTMETRICW metrics{};
        measure(font, &extent, &metrics);
        int const ink = metrics.tmAscent - metrics.tmInternalLeading;

        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        // 下地は黒。被覆率は白で塗られた分として読まれる
        SetTextColor(dc, RGB(255, 255, 255));
        // 字面の真ん中を枠の真ん中に合わせる。TextOut が受けるのは字面の上では
        // なく行の上なので、上の余白ぶんだけ持ち上げる。
        int const x = RoundToInt((box - extent.cx) / 2.0);
        int const y = RoundToInt((box - ink) / 2.0) -
                      (metrics.tmAscent - ink);
        TextOutW(dc, x, y, text.c_str(), (int)text.size());
        SelectObject(dc, old_font);
        DeleteObject(font);
    });

    image::Bgra glyph = image::Premultiply(fg, digits, size, size);
    image::CompositeOver(out, glyph);
    return out;
}

image::Bgra AlbumArt(std::vector<uint8_t> const& data, int size, int radius, Rgb fg,
                     Rgb bg, bool keep_aspect) {
    if (!data.empty()) {
        if (auto decoded = image::Decode(data)) {
            image::Rgb888 img = image::TrimPadding(*decoded);
            if (!keep_aspect) img = image::CenterSquare(img);
            if (!img.empty()) {
                SIZE const target = Fit(img.width, img.height, size);
                double const factor = (double)target.cx / img.width;
                bool const enlarging = factor >= kUpscaleSharpenFrom;
                if (enlarging) {
                    image::BlurChroma(img, Clamp(factor * kChromaBlurRatio,
                                                 kChromaBlurLow, kChromaBlurHigh));
                }
                img = image::Resize(img, (int)target.cx, (int)target.cy);
                if (enlarging) {
                    image::SharpenLuma(img,
                                       Clamp(factor * kSharpenRadiusRatio,
                                             kSharpenRadiusLow, kSharpenRadiusHigh),
                                       kSharpenPercent, kSharpenThreshold);
                }
                image::Mask const mask =
                    image::RoundedRectMask(img.width, img.height, radius);
                return image::Premultiply(img, mask);
            }
        }
    }
    return NoteIcon(size, fg, bg, radius);
}

std::optional<image::Rgb888> Fingerprint(std::vector<uint8_t> const& data) {
    if (data.empty()) return std::nullopt;
    auto decoded = image::Decode(data);
    if (!decoded) return std::nullopt;
    return image::Resize(*decoded, kPrintSize, kPrintSize);
}

bool SamePicture(std::optional<image::Rgb888> const& a,
                 std::optional<image::Rgb888> const& b) {
    // 「同じ絵」と見なす、画素値の平均差の上限 (0-255)。
    //
    // 同じ絵が 150x150 -> 120x120 と縮んで届くとき、縮んだ方は JPEG の畳み直しで
    // 崩れているので、指紋どうしでも差が出る。実測では、写真のようなアートで
    // 0.30、細い線が斜めに走るアートで 6.64 だった。一方、別の絵どうしは 49.9 が
    // 最小で、たいてい 100 を超える。6.0 では線の細いアートを「別の絵」と取り
    // 違えて、粗い方に差し替えてしまっていた。
    //
    // 取り違えても害が小さいのは「同じ」と見た側 (いま持っている大きい方を使い
    // 続けるだけ)。「別」と見た側は前の曲のアートを出し続けることになる。両者の
    // 間が十分に空いているので、実測のいちばん悪い値の 3 倍あたりに置く。
    constexpr double kSameLimit = 20.0;
    if (!a || !b) return false;
    return image::MeanAbsDiff(*a, *b) <= kSameLimit;
}

}  // namespace artwork
