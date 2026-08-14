"""タスクバー バーとトレイ アイコンで共通に使う描画まわりの小道具。"""

from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont

TEXT_FAMILIES = ("Yu Gothic UI", "Meiryo UI", "Segoe UI")
ICON_FAMILIES = ("Segoe Fluent Icons", "Segoe MDL2 Assets")

# Segoe Fluent Icons / Segoe MDL2 Assets のコード ポイント
MDL2_GLYPHS = {
    "prev": "",
    "next": "",
    "play": "",
    "pause": "",
}
PLAIN_GLYPHS = {
    "prev": "⏮",
    "next": "⏭",
    "play": "▶",
    "pause": "⏸",
}


def pick_font(root: tk.Misc, candidates: tuple[str, ...], fallback: str) -> str:
    available = {name.lower() for name in tkfont.families(root)}
    for name in candidates:
        if name.lower() in available:
            return name
    return fallback


class IconSet:
    """アイコン フォントと、そこから引くグリフ。"""

    def __init__(self, root: tk.Misc) -> None:
        self.family = pick_font(root, ICON_FAMILIES, "Segoe UI Symbol")
        self._mdl2 = self.family != "Segoe UI Symbol"

    def glyph(self, name: str) -> str:
        return (MDL2_GLYPHS if self._mdl2 else PLAIN_GLYPHS)[name]


def elide(text: str, font: tkfont.Font, max_width: int) -> str:
    """入りきらない文字列を末尾 "…" で省略する。"""
    if not text or max_width <= 0 or font.measure(text) <= max_width:
        return text
    limit = max_width - font.measure("…")
    if limit <= 0:
        return "…"
    low, high = 0, len(text)
    while low < high:
        mid = (low + high + 1) // 2
        if font.measure(text[:mid]) <= limit:
            low = mid
        else:
            high = mid - 1
    return text[:low].rstrip() + "…"



def luminance(color: str) -> float:
    """#rrggbb の明るさ (0..1)。文字色を決めるのに使う。"""
    r, g, b = (int(color[i : i + 2], 16) / 255 for i in (1, 3, 5))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b
