"""再生中の曲を表示する、枠なしのミニ パネル。"""

from __future__ import annotations

import time
import tkinter as tk
from tkinter import font as tkfont
from typing import Callable, Optional

from PIL import ImageTk

import icons
import winapi
from media_session import NowPlaying
from theme import Palette, round_window_corners, ui_scale, work_area
from uiutil import IconSet, TEXT_FAMILIES, elide, format_time, pick_font

# 96 DPI 基準のレイアウト (実際には DPI 倍率をかけて使う)
W, H = 420, 188
RADIUS = 12
ART_X, ART_Y, ART = 18, 18, 92
ART_RADIUS = 8
TEXT_X, TEXT_R = 126, 402
TITLE_Y, ARTIST_Y, META_Y = 26, 52, 74
PROG_X0, PROG_X1, PROG_Y, PROG_H = 18, 402, 120, 5
TIME_Y = 130
BTN_CY = 163
PREV_CX, PLAY_CX, NEXT_CX = 174, 210, 246
PIN_CX, CLOSE_CX = 352, 386
BTN_R, SMALL_R = 17, 13
MARGIN = 12  # 画面端からの余白

EMPTY_TITLE = "再生中の音楽はありません"
EMPTY_ARTIST = "対応プレーヤーで再生すると、ここに表示されます"


class Panel:
    """トレイから開閉するミニ プレーヤー パネル。"""

    def __init__(
        self,
        root: tk.Tk,
        controller,
        config,
        on_visibility: Optional[Callable[[bool], None]] = None,
    ) -> None:
        self.root = root
        self.controller = controller
        self.config = config
        self.on_visibility = on_visibility

        self.palette = Palette.current()
        self.scale = ui_scale()
        self.state = NowPlaying()
        self.visible = False

        self._art_key: Optional[str] = None
        self._art_image = None  # ImageTk への参照を保持
        self._hover: Optional[str] = None
        self._pressed: Optional[str] = None
        self._shown_at = 0.0
        self._drag_origin = (0, 0)
        self._drag: Optional[tuple[int, int]] = None
        self._region_applied = False

        self.win = tk.Toplevel(root)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg=self.palette.card)
        self.win.title("MuuTask")

        text_family = pick_font(root, TEXT_FAMILIES, "Segoe UI")
        self.icons = IconSet(root)
        icon_family = self.icons.family
        self.f_title = tkfont.Font(root=root, family=text_family, size=-self.s(16), weight="bold")
        self.f_artist = tkfont.Font(root=root, family=text_family, size=-self.s(12))
        self.f_meta = tkfont.Font(root=root, family=text_family, size=-self.s(11))
        self.f_time = tkfont.Font(root=root, family=text_family, size=-self.s(11))
        self.f_icon = tkfont.Font(root=root, family=icon_family, size=-self.s(17))
        self.f_icon_small = tkfont.Font(root=root, family=icon_family, size=-self.s(12))
        self.f_play = tkfont.Font(root=root, family=icon_family, size=-self.s(15))

        self.canvas = tk.Canvas(
            self.win,
            width=self.s(W),
            height=self.s(H),
            bg=self.palette.card,
            highlightthickness=0,
            bd=0,
        )
        self.canvas.pack()

        self._hitboxes: dict[str, tuple[int, int, int, int]] = {}
        self._build()
        self._bind()
        self.update_state(NowPlaying())

    # ------------------------------------------------------------------ 補助

    def s(self, value: float) -> int:
        return int(round(value * self.scale))

    # ------------------------------------------------------------------ 組み立て

    def _build(self) -> None:
        p, s = self.palette, self.s
        c = self.canvas

        c.create_rectangle(
            0, 0, s(W) - 1, s(H) - 1, outline=p.border, width=1, tags="frame"
        )
        self.art_item = c.create_image(s(ART_X), s(ART_Y), anchor="nw", tags="art")
        self.title_item = c.create_text(
            s(TEXT_X), s(TITLE_Y), anchor="nw", font=self.f_title, fill=p.title
        )
        self.artist_item = c.create_text(
            s(TEXT_X), s(ARTIST_Y), anchor="nw", font=self.f_artist, fill=p.body
        )
        self.meta_item = c.create_text(
            s(TEXT_X), s(META_Y), anchor="nw", font=self.f_meta, fill=p.muted
        )

        self.track_item = c.create_line(
            s(PROG_X0), s(PROG_Y), s(PROG_X1), s(PROG_Y),
            width=s(PROG_H), fill=p.track, capstyle=tk.ROUND,
        )
        self.fill_item = c.create_line(
            s(PROG_X0), s(PROG_Y), s(PROG_X0), s(PROG_Y),
            width=s(PROG_H), fill=p.accent, capstyle=tk.ROUND, state="hidden",
        )
        knob = s(4)
        self.knob_item = c.create_oval(
            s(PROG_X0) - knob, s(PROG_Y) - knob, s(PROG_X0) + knob, s(PROG_Y) + knob,
            fill=p.accent, outline="", state="hidden",
        )
        self.time_item = c.create_text(
            s(PROG_X0), s(TIME_Y), anchor="nw", font=self.f_time, fill=p.muted
        )
        self.total_item = c.create_text(
            s(PROG_X1), s(TIME_Y), anchor="ne", font=self.f_time, fill=p.muted
        )

        r = s(BTN_R)
        self.play_bg = c.create_oval(
            s(PLAY_CX) - r, s(BTN_CY) - r, s(PLAY_CX) + r, s(BTN_CY) + r,
            fill=p.accent, outline="",
        )
        self.prev_item = c.create_text(
            s(PREV_CX), s(BTN_CY), font=self.f_icon, fill=p.body, text=self.icons.glyph("prev")
        )
        self.play_item = c.create_text(
            s(PLAY_CX), s(BTN_CY), font=self.f_play, fill=p.card, text=self.icons.glyph("play")
        )
        self.next_item = c.create_text(
            s(NEXT_CX), s(BTN_CY), font=self.f_icon, fill=p.body, text=self.icons.glyph("next")
        )
        self.pin_item = c.create_text(
            s(PIN_CX), s(BTN_CY), font=self.f_icon_small, fill=p.muted, text=self.icons.glyph("pin")
        )
        self.close_item = c.create_text(
            s(CLOSE_CX), s(BTN_CY), font=self.f_icon_small, fill=p.muted,
            text=self.icons.glyph("close"),
        )

        self._hitboxes = {
            "prev": self._box(PREV_CX, BTN_CY, BTN_R),
            "play": self._box(PLAY_CX, BTN_CY, BTN_R),
            "next": self._box(NEXT_CX, BTN_CY, BTN_R),
            "pin": self._box(PIN_CX, BTN_CY, SMALL_R),
            "close": self._box(CLOSE_CX, BTN_CY, SMALL_R),
            "seek": (s(PROG_X0) - s(6), s(PROG_Y) - s(9), s(PROG_X1) + s(6), s(PROG_Y) + s(9)),
        }

    def _box(self, cx: int, cy: int, r: int) -> tuple[int, int, int, int]:
        s = self.s
        return (s(cx - r), s(cy - r), s(cx + r), s(cy + r))

    def _bind(self) -> None:
        c = self.canvas
        c.bind("<Motion>", self._on_motion)
        c.bind("<Leave>", lambda _e: self._set_hover(None))
        c.bind("<Button-1>", self._on_press)
        c.bind("<B1-Motion>", self._on_drag)
        c.bind("<ButtonRelease-1>", self._on_release)
        self.win.bind("<FocusOut>", self._on_focus_out)
        self.win.bind("<Escape>", lambda _e: self.hide())

    # ------------------------------------------------------------------ 入力

    def _hit(self, x: int, y: int) -> Optional[str]:
        for name, (x0, y0, x1, y1) in self._hitboxes.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                if name in ("prev", "next", "play", "seek") and not self._enabled(name):
                    return None
                return name
        return None

    def _enabled(self, name: str) -> bool:
        st = self.state
        return {
            "prev": st.can_prev,
            "next": st.can_next,
            "play": st.can_play or st.can_pause,
            "seek": st.can_seek and st.duration > 0,
        }.get(name, True)

    def _on_motion(self, event) -> None:
        self._set_hover(self._hit(event.x, event.y))

    def _set_hover(self, name: Optional[str]) -> None:
        if name == self._hover:
            return
        self._hover = name
        self.canvas.configure(cursor="hand2" if name else "")
        self._paint_buttons()

    def _on_press(self, event) -> None:
        self._pressed = self._hit(event.x, event.y)
        if self._pressed is None:
            self._drag = (event.x_root, event.y_root)
            self._drag_origin = (self.win.winfo_x(), self.win.winfo_y())

    def _on_drag(self, event) -> None:
        if self._drag is None:
            return
        dx = event.x_root - self._drag[0]
        dy = event.y_root - self._drag[1]
        self.win.geometry(f"+{self._drag_origin[0] + dx}+{self._drag_origin[1] + dy}")

    def _on_release(self, event) -> None:
        if self._drag is not None:
            self._drag = None
            self.config.x, self.config.y = self.win.winfo_x(), self.win.winfo_y()
            self.config.save()
            return
        name = self._hit(event.x, event.y)
        if name is None or name != getattr(self, "_pressed", None):
            return
        if name == "prev":
            self.controller.previous_track()
        elif name == "next":
            self.controller.next_track()
        elif name == "play":
            self.controller.toggle_play_pause()
        elif name == "pin":
            self.set_pinned(not self.config.pinned)
        elif name == "close":
            self.hide()
        elif name == "seek":
            x0, x1 = self.s(PROG_X0), self.s(PROG_X1)
            ratio = min(1.0, max(0.0, (event.x - x0) / max(1, x1 - x0)))
            self.controller.seek(self.state.duration * ratio)

    def _on_focus_out(self, _event) -> None:
        # 表示直後のフォーカス移動で閉じてしまわないよう少し猶予を持たせる
        if self.visible and not self.config.pinned and time.monotonic() - self._shown_at > 0.4:
            self.hide()

    # ------------------------------------------------------------------ 表示制御

    def _place(self) -> None:
        width, height = self.s(W), self.s(H)
        left, top, right, bottom = work_area()
        x, y = self.config.x, self.config.y
        if x is None or y is None or not (left - 40 <= x <= right - 40 and top - 40 <= y <= bottom - 40):
            x = right - width - self.s(MARGIN)
            y = bottom - height - self.s(MARGIN)
        self.win.geometry(f"{width}x{height}+{int(x)}+{int(y)}")

    def show(self) -> None:
        self._place()
        self.win.deiconify()
        self.win.lift()
        self.win.attributes("-topmost", True)
        self._shown_at = time.monotonic()
        self.visible = True
        try:
            self.win.focus_force()
        except tk.TclError:
            pass
        if not self._region_applied:
            self.win.update_idletasks()
            round_window_corners(
                winapi.toplevel_hwnd(self.win.winfo_id()),
                self.s(W),
                self.s(H),
                self.s(RADIUS),
            )
            self._region_applied = True
        if self.on_visibility:
            self.on_visibility(True)

    def hide(self) -> None:
        self.win.withdraw()
        self.visible = False
        self._set_hover(None)
        if self.on_visibility:
            self.on_visibility(False)

    def toggle(self) -> None:
        self.hide() if self.visible else self.show()

    def set_pinned(self, pinned: bool) -> None:
        self.config.pinned = pinned
        self.config.save()
        if pinned and not self.visible:
            self.show()
        self._paint_buttons()

    # ------------------------------------------------------------------ 描画

    def update_state(self, state: NowPlaying) -> None:
        self.state = state
        c = self.canvas
        p = self.palette

        has_media = state.has_media and bool(state.title or state.artist)
        title = state.title if has_media else EMPTY_TITLE
        artist = state.artist if has_media else EMPTY_ARTIST
        meta = " · ".join(x for x in (state.album, state.app_name) if x) if has_media else ""

        width = self.s(TEXT_R - TEXT_X)
        c.itemconfigure(self.title_item, text=elide(title, self.f_title, width))
        c.itemconfigure(self.artist_item, text=elide(artist, self.f_artist, width))
        c.itemconfigure(self.meta_item, text=elide(meta, self.f_meta, width))

        art_key = f"{state.track_key}|{p.dark}|{state.status}"
        if art_key != self._art_key:
            self._art_key = art_key
            size = self.s(ART)
            image = icons.album_art(
                state.thumbnail, size, self.s(ART_RADIUS), p.muted, p.track
            )
            if has_media and not state.is_playing:
                image = icons.dim(image, 0.35)
            self._art_image = ImageTk.PhotoImage(icons.flatten(image, p.card))
            c.itemconfigure(self.art_item, image=self._art_image)

        c.itemconfigure(
            self.play_item,
            text=self.icons.glyph("pause" if state.is_playing else "play"),
        )
        self._paint_buttons()
        self.update_progress()

    def update_progress(self) -> None:
        c, p, s = self.canvas, self.palette, self.s
        state = self.state
        duration = state.duration
        if not state.has_media or duration <= 0:
            c.itemconfigure(self.fill_item, state="hidden")
            c.itemconfigure(self.knob_item, state="hidden")
            c.itemconfigure(self.time_item, text="")
            c.itemconfigure(self.total_item, text="ライブ" if state.has_media else "")
            return

        position = state.live_position()
        ratio = min(1.0, max(0.0, position / duration))
        x0, x1 = s(PROG_X0), s(PROG_X1)
        x = x0 + (x1 - x0) * ratio
        c.itemconfigure(self.fill_item, state="normal")
        c.coords(self.fill_item, x0, s(PROG_Y), max(x0 + 1, x), s(PROG_Y))
        knob = s(5)
        c.itemconfigure(self.knob_item, state="normal")
        c.coords(self.knob_item, x - knob, s(PROG_Y) - knob, x + knob, s(PROG_Y) + knob)
        c.itemconfigure(self.time_item, text=format_time(position))
        c.itemconfigure(self.total_item, text=format_time(duration))

    def _paint_buttons(self) -> None:
        c, p = self.canvas, self.palette
        for name, item in (
            ("prev", self.prev_item),
            ("next", self.next_item),
        ):
            if not self._enabled(name):
                color = p.disabled
            elif self._hover == name:
                color = p.title
            else:
                color = p.body
            c.itemconfigure(item, fill=color)

        play_on = self._enabled("play")
        if not play_on:
            c.itemconfigure(self.play_bg, fill=p.track)
            c.itemconfigure(self.play_item, fill=p.disabled)
        else:
            c.itemconfigure(
                self.play_bg, fill=p.accent_hover if self._hover == "play" else p.accent
            )
            c.itemconfigure(self.play_item, fill=p.card)

        pinned = self.config.pinned
        c.itemconfigure(
            self.pin_item,
            text=self.icons.glyph("unpin" if pinned else "pin"),
            fill=p.accent if pinned else (p.title if self._hover == "pin" else p.muted),
        )
        c.itemconfigure(
            self.close_item, fill=p.title if self._hover == "close" else p.muted
        )
