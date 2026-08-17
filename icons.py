"""アルバム アートとフォールバック アイコンを Pillow で描く。"""

from __future__ import annotations

import io
from typing import Optional

from PIL import Image, ImageDraw, ImageFilter

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


def album_art(data: Optional[bytes], size: int, radius: int, fg: str, bg: str) -> Image.Image:
    """アルバム アート画像。取得できなければ音符アイコンを返す。"""
    if data:
        try:
            image = Image.open(io.BytesIO(data))
            image.load()
            image = image.convert("RGB")
            # 中央を正方形に切り出す
            width, height = image.size
            side = min(width, height)
            image = image.crop(
                (
                    (width - side) // 2,
                    (height - side) // 2,
                    (width - side) // 2 + side,
                    (height - side) // 2 + side,
                )
            )
            factor = size / side if side else 1.0
            enlarging = factor >= UPSCALE_SHARPEN_FROM
            if enlarging:
                image = _clean_chroma(image, factor)
            image = image.resize((size, size), Image.LANCZOS)
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
