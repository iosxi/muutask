"""バーにマウスを乗せたときに、アルバム アートを大きく出す小窓。

タスクバーのボタンにカーソルを載せると出るプレビューと同じ位置・同じくらいの
大きさに合わせている。Windows 11 のプレビューは XAML の中で描かれていて
ウィンドウとして掴めない (クラス名で辿れるのは 1x1 の
ThumbnailDeviceHelperWnd だけだった) ため、高さは実測ではなく既定値を持ち、
config.json の popup_height で変えられるようにしている。
"""

from __future__ import annotations

import tkinter as tk
from typing import Optional

from PIL import ImageTk

import icons
import theme
import winapi

#: 小窓の高さ (96 DPI 基準の px)。Windows のプレビューを超えない大きさ
DEFAULT_HEIGHT = 180
#: 内側の余白 (96 DPI 基準の px)
PAD = 10
#: タスクバーとの間隔 (96 DPI 基準の px)
GAP = 8
#: カーソルを乗せてから出るまで (ms)。Windows のプレビューに合わせる
DELAY = 450


class ArtPopup:
    """アルバム アートだけを見せる、枠なしの小窓。"""

    def __init__(self, root: tk.Tk, config, scale: float) -> None:
        self.root = root
        self.config = config
        self.scale = scale
        self.visible = False

        self._image = None  # ImageTk への参照を保持する
        self._key: Optional[str] = None
        self._size = 0
        self._hwnd = 0
        self._rounded = False

        self.palette = theme.Palette.current()
        self.win = tk.Toplevel(root)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg=self.palette.card)
        self.canvas = tk.Canvas(
            self.win, highlightthickness=0, bd=0, bg=self.palette.card
        )
        self.canvas.pack()
        self._item = self.canvas.create_image(0, 0, anchor="nw")

    # ------------------------------------------------------------------ 大きさ

    def _side(self) -> int:
        """小窓の一辺 (正方形)。アルバム アートが正方形なので縦横を揃える。"""
        height = self.config.popup_height or DEFAULT_HEIGHT
        return max(80, round(height * self.scale))

    # ------------------------------------------------------------------ 表示

    def show(self, state, bar_rect: tuple[int, int, int, int]) -> None:
        """バーの真上に出す。bar_rect は (x, y, 幅, 高さ)。"""
        side = self._side()
        pad = max(4, round(PAD * self.scale))
        art = side - pad * 2

        key = f"{state.art_key}|{art}"
        if key != self._key or self._size != side:
            self._key, self._size = key, side
            image = icons.album_art(
                state.thumbnail, art, max(4, art // 16),
                self.palette.muted, self.palette.track,
            )
            self._image = ImageTk.PhotoImage(icons.flatten(image, self.palette.card))
            self.canvas.configure(width=side, height=side)
            self.canvas.coords(self._item, pad, pad)
            self.canvas.itemconfigure(self._item, image=self._image)

        x, y, width, _height = bar_rect
        taskbar = winapi.taskbar_rect()
        top = taskbar[1] if taskbar else y
        gap = max(2, round(GAP * self.scale))

        left = x + width // 2 - side // 2
        left = max(0, min(left, self.win.winfo_screenwidth() - side))
        self.win.geometry(f"{side}x{side}+{left}+{top - gap - side}")

        if not self.visible:
            self.win.deiconify()
            self.win.update_idletasks()
            self.visible = True
            if not self._hwnd:
                self._hwnd = winapi.toplevel_hwnd(self.win.winfo_id())
                # クリックしても前に出ないように。バーと同じ扱いにする
                winapi.make_tool_window(self._hwnd)
            if not self._rounded:
                theme.round_window_corners(
                    self._hwnd, side, side, max(4, round(8 * self.scale))
                )
                self._rounded = True
        winapi.raise_to_top(self._hwnd)

    def hide(self) -> None:
        if self.visible:
            self.win.withdraw()
            self.visible = False
