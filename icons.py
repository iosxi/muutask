"""アルバム アートとフォールバック アイコンを Pillow で描く。"""

from __future__ import annotations

import io
from typing import Optional

from PIL import Image, ImageDraw, ImageFilter, ImageStat

SUPERSAMPLE = 4

#: この倍率より大きく引き伸ばすときは、色ノイズをならして輪郭を締め直す。
#: Chrome 経由のアートは 150x150 までしか来ないので、大きな小窓では必ず通る
UPSCALE_SHARPEN_FROM = 1.15
#: 締め直しの半径。元の 1 画素ぶんの半分あたりに置くと、輪郭だけに効く
SHARPEN_RADIUS_RATIO = 0.5
#: 半径の下限と上限 (px)。効かなすぎ・輪郭が浮きすぎるのを防ぐ
SHARPEN_RADIUS_RANGE = (0.8, 3.0)
#: 締め直しの強さ (%) と、無視する差 (0-255)。閾値を置くと平らな面が荒れない
SHARPEN_PERCENT = 110
SHARPEN_THRESHOLD = 2
#: 色ノイズをならす半径 (元画像の画素で数える) と、その下限・上限。
#: もらえるアートは JPEG を通ってきた小さい絵なので、平らな面に緑や紫の
#: 斑点が乗っている。引き伸ばすとそれが粒として見えるが、色だけならせば
#: 輪郭の鋭さを保ったまま消せる
CHROMA_BLUR_RATIO = 0.3
CHROMA_BLUR_RANGE = (0.5, 1.2)

#: 縁の帯を落とすときの、その一列の「平ら」さ (画素値のばらつきの上限)
PAD_FLATNESS = 3.0
#: 角の色とどれだけ近ければ帯と見なすか (0-255)
PAD_NEARNESS = 6
#: 片側で落とせる割合の上限。絵そのものを食べないための歯止め
PAD_MAX_TRIM = 0.30


def _hex_to_rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i : i + 2], 16) for i in (1, 3, 5))


def rounded(image: Image.Image, radius: int) -> Image.Image:
    """角を丸くした RGBA 画像を返す。"""
    size = image.size
    big = (size[0] * SUPERSAMPLE, size[1] * SUPERSAMPLE)
    mask = Image.new("L", big, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, big[0] - 1, big[1] - 1), radius=radius * SUPERSAMPLE, fill=255
    )
    mask = mask.resize(size, Image.LANCZOS)
    out = image.convert("RGBA")
    out.putalpha(mask)
    return out


def note_icon(size: int, fg: str, bg: Optional[str] = None, radius: int = 6) -> Image.Image:
    """音符アイコン。bg を渡すと角丸の下地を敷く。"""
    scale = size * SUPERSAMPLE
    canvas = Image.new("RGBA", (scale, scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)

    if bg is not None:
        draw.rounded_rectangle(
            (0, 0, scale - 1, scale - 1),
            radius=radius * SUPERSAMPLE,
            fill=_hex_to_rgb(bg) + (255,),
        )

    def px(value: float) -> float:
        return value / 100.0 * scale

    color = _hex_to_rgb(fg) + (255,)
    # 符頭 2 つ
    draw.ellipse((px(16), px(60), px(48), px(85)), fill=color)
    draw.ellipse((px(56), px(50), px(88), px(75)), fill=color)
    # 符幹
    draw.rectangle((px(42), px(20), px(48), px(73)), fill=color)
    draw.rectangle((px(82), px(10), px(88), px(63)), fill=color)
    # 連桁
    draw.polygon(
        [(px(42), px(20)), (px(88), px(10)), (px(88), px(26)), (px(42), px(36))],
        fill=color,
    )
    return canvas.resize((size, size), Image.LANCZOS)


def _trim_padding(image: Image.Image) -> Image.Image:
    """縁の帯を落とす。

    YouTube Music は 4:3 のジャケットを正方形のキャンバスに置いて渡してくる
    ことがある (実測では (18,18,18) の帯で埋めてあった)。帯を含めたまま扱うと、
    小窓の中でジャケットが小さくなり、上下に地色でない黒が残る。

    角の色と同じで、かつ平らな列を外側から落とす。絵そのものが縁まで一色の
    ときに食べ過ぎないよう、片側 30% で打ち切る。
    """
    width, height = image.size
    corner = image.getpixel((0, 0))

    def is_pad(box) -> bool:
        stat = ImageStat.Stat(image.crop(box))
        return max(stat.stddev) <= PAD_FLATNESS and all(
            abs(stat.mean[i] - corner[i]) <= PAD_NEARNESS for i in range(3)
        )

    top = bottom = left = right = 0
    while top < height * PAD_MAX_TRIM and is_pad((0, top, width, top + 1)):
        top += 1
    while bottom < height * PAD_MAX_TRIM and is_pad(
        (0, height - 1 - bottom, width, height - bottom)
    ):
        bottom += 1
    while left < width * PAD_MAX_TRIM and is_pad((left, 0, left + 1, height)):
        left += 1
    while right < width * PAD_MAX_TRIM and is_pad(
        (width - 1 - right, 0, width - right, height)
    ):
        right += 1
    if not (top or bottom or left or right):
        return image
    return image.crop((left, top, width - right, height - bottom))


def _fit(size: tuple[int, int], box: int) -> tuple[int, int]:
    """長辺が box に収まる大きさ。縦横の比はそのまま。"""
    width, height = size
    if width >= height:
        return box, max(1, round(box * height / width))
    return max(1, round(box * width / height)), box


def _center_square(image: Image.Image) -> Image.Image:
    """中央を正方形に切り出す。"""
    width, height = image.size
    side = min(width, height)
    x, y = (width - side) // 2, (height - side) // 2
    return image.crop((x, y, x + side, y + side))


def _clean_chroma(image: Image.Image, factor: float) -> Image.Image:
    """色ノイズをならす。引き伸ばす前の、小さいうちにかける。

    JPEG は色を間引いて畳むので、届くアートの平らな面には緑や紫の斑点が
    残っている。引き伸ばすとそれが粒になって浮く。色 (Cb/Cr) だけをぼかし、
    輝度 (Y) には触らないので、輪郭の鋭さは落ちない。
    """
    low, high = CHROMA_BLUR_RANGE
    blur = ImageFilter.GaussianBlur(min(high, max(low, factor * CHROMA_BLUR_RATIO)))
    y, cb, cr = image.convert("YCbCr").split()
    return Image.merge("YCbCr", (y, cb.filter(blur), cr.filter(blur))).convert("RGB")


def _sharpen_luma(image: Image.Image, factor: float) -> Image.Image:
    """引き伸ばした画像の輪郭を締め直す。無い解像度は戻らないが、ぼやけた
    感じはかなり収まる。

    輝度だけを締める。RGB のまま締めると、せっかくならした色の縁が立ち直り、
    輪郭に色が付いて見える。
    """
    low, high = SHARPEN_RADIUS_RANGE
    radius = min(high, max(low, factor * SHARPEN_RADIUS_RATIO))
    y, cb, cr = image.convert("YCbCr").split()
    y = y.filter(
        ImageFilter.UnsharpMask(
            radius=radius, percent=SHARPEN_PERCENT, threshold=SHARPEN_THRESHOLD
        )
    )
    return Image.merge("YCbCr", (y, cb, cr)).convert("RGB")


def album_art(
    data: Optional[bytes],
    size: int,
    radius: int,
    fg: str,
    bg: str,
    keep_aspect: bool = False,
) -> Image.Image:
    """アルバム アート画像。取得できなければ音符アイコンを返す。

    もらえる絵は正方形とは限らない。YouTube の動画では 150x83 (16:9) が
    そのまま来る (実測)。keep_aspect を立てると縦横の比を保ったまま、長辺が
    size に収まる大きさで返す。立てないときは中央を正方形に切り出す
    (バーの小さな枠やトレイ アイコンのように、正方形しか置けない場所用)。
    """
    if data:
        try:
            image = Image.open(io.BytesIO(data))
            image.load()
            image = _trim_padding(image.convert("RGB"))
            if not keep_aspect:
                image = _center_square(image)
            target = _fit(image.size, size)
            factor = target[0] / image.size[0]
            enlarging = factor >= UPSCALE_SHARPEN_FROM
            if enlarging:
                image = _clean_chroma(image, factor)
            image = image.resize(target, Image.LANCZOS)
            if enlarging:
                image = _sharpen_luma(image, factor)
            return rounded(image, radius)
        except (OSError, ValueError):
            pass
    return note_icon(size, fg, bg, radius)


def flatten(image: Image.Image, bg: str) -> Image.Image:
    """透過を背景色で潰した RGB 画像 (Tk 用)。"""
    base = Image.new("RGB", image.size, _hex_to_rgb(bg))
    base.paste(image, (0, 0), image)
    return base


def dim(image: Image.Image, amount: float) -> Image.Image:
    """画像を暗くする (一時停止中の表現に使う)。"""
    out = image.convert("RGBA")
    black = Image.new("RGBA", out.size, (0, 0, 0, int(255 * amount)))
    merged = Image.alpha_composite(out, black)
    merged.putalpha(out.getchannel("A"))  # 角の透過を保つ
    return merged
