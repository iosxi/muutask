"""アルバム アートとフォールバック アイコンを Pillow で描く。"""

from __future__ import annotations

import io
from typing import Optional

from PIL import Image, ImageDraw

SUPERSAMPLE = 4


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
            ).resize((size, size), Image.LANCZOS)
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
