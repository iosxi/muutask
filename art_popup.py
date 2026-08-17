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
        self._box = 0  # 長辺の上限 (DPI をかけたあとの px)
        self._size = (0, 0)  # いまの小窓の大きさ
        self._hwnd = 0
        self._rounded = (0, 0)  # 角を丸めたときの大きさ

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

    def _limit(self) -> int:
        """小窓に収める長辺の上限。

        アートは正方形とは限らない (YouTube の動画では 16:9 が来る) ので、
        popup_height は「一辺」ではなく「長辺の上限」として扱う。こうすると
        設定した大きさを超えずに、絵の全体が入る。
        """
        height = self.config.popup_height or DEFAULT_HEIGHT
        return max(80, round(height * self.scale))

    # ------------------------------------------------------------------ 表示

    def show(self, state, bar_rect: tuple[int, int, int, int]) -> None:
        """バーの真上に出す。bar_rect は (x, y, 幅, 高さ)。"""
        box = self._limit()
        pad = max(4, round(PAD * self.scale))
        art = box - pad * 2

        key = f"{state.art_key}|{art}"
        if key != self._key or self._box != box:
            self._key, self._box = key, box
            image = icons.album_art(
                state.thumbnail, art, max(4, art // 16),
                self.palette.muted, self.palette.track, keep_aspect=True,
            )
            self._image = ImageTk.PhotoImage(icons.flatten(image, self.palette.card))
            self._size = (image.width + pad * 2, image.height + pad * 2)
            self.canvas.configure(width=self._size[0], height=self._size[1])
            self.canvas.coords(self._item, pad, pad)
            self.canvas.itemconfigure(self._item, image=self._image)

        win_w, win_h = self._size
        x, y, bar_width, _bar_height = bar_rect
        taskbar = winapi.taskbar_rect()
        top = taskbar[1] if taskbar else y
        gap = max(2, round(GAP * self.scale))

        left = x + bar_width // 2 - win_w // 2
        left = max(0, min(left, self.win.winfo_screenwidth() - win_w))
        self.win.geometry(f"{win_w}x{win_h}+{left}+{top - gap - win_h}")

        if not self.visible:
            self.win.deiconify()
            self.win.update_idletasks()
            self.visible = True
            if not self._hwnd:
                self._hwnd = winapi.toplevel_hwnd(self.win.winfo_id())
                # クリックしても前に出ないように。バーと同じ扱いにする
                winapi.make_tool_window(self._hwnd)
        # 絵の形が変わると小窓の大きさも変わる。角はそのたびに丸め直す
        if self._hwnd and self._rounded != self._size:
            theme.round_window_corners(
                self._hwnd, win_w, win_h, max(4, round(8 * self.scale))
            )
            self._rounded = self._size
        winapi.raise_to_top(self._hwnd)

    def hide(self) -> None:
        if self.visible:
            self.win.withdraw()
            self.visible = False
