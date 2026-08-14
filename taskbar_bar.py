"""タスクバーの空きスペースに重ねて表示する、曲情報 + 操作ボタンのバー。

Windows 11 ではタスクバーへのツール バー追加 (デスク バンド) が廃止されているため、
タスクバーとぴったり重なる最前面ウィンドウを置き、位置と重なり順を定期的に
追従させることで「タスクバーの中にいる」ように見せている。
"""

from __future__ import annotations

import os
import time
import tkinter as tk
from tkinter import font as tkfont
from typing import Callable, Optional

from PIL import ImageTk

import icons
import winapi
from media_session import NowPlaying
from uiutil import IconSet, TEXT_FAMILIES, luminance, mix, pick_font

#: 既定のバー幅 (96 DPI 基準の px)
DEFAULT_WIDTH = 340
#: 隣の要素との間隔 (96 DPI 基準の px)
DEFAULT_GAP = 8
#: タスクバー内での置き場所 (設定値, 表示名)
BAR_ANCHORS = (
    ("left", "左端"),
    ("start", "スタート ボタンの左"),
    ("tray", "通知領域の左"),
)
#: メニューから選べる幅 (96 DPI 基準の px, 表示名)
BAR_WIDTHS = (
    (240, "狭い"),
    (340, "標準"),
    (460, "広い"),
    (600, "とても広い"),
)
#: これより狭い場所には置かない (96 DPI 基準の px)
MIN_WIDTH = 120

EMPTY_TITLE = "再生中の音楽はありません"

#: 文字送りの速さ (96 DPI 基準の px/秒)
SCROLL_SPEED = 34.0
#: 動き出すまでの待ち時間 (秒)。頭を読ませてから流す
SCROLL_DELAY = 1.6
#: 繰り返しの間に入れる空き
SCROLL_GAP = "　　　　"

#: MUUTASK_DEBUG=1 で追従処理のログを出す
DEBUG = bool(os.environ.get("MUUTASK_DEBUG"))


class TaskbarBar:
    """タスクバーに重ねて出す横長のミニ バー。"""

    def __init__(
        self,
        root: tk.Tk,
        controller,
        config,
        scale: float,
        on_context_menu: Callable[[int, int], None],
    ) -> None:
        self.root = root
        self.controller = controller
        self.config = config
        self.scale = scale
        self.on_context_menu = on_context_menu

        self.state = NowPlaying()
        self.visible = False

        self._geometry: Optional[tuple[int, int, int, int]] = None
        self._art_key: Optional[str] = None
        self._art_image = None
        self._hwnd = 0
        self._covered_since: Optional[float] = None
        self._hover: Optional[str] = None
        self._pressed: Optional[str] = None
        self._hitboxes: dict[str, tuple[int, int, int, int]] = {}
        # 流れる文字まわり
        self._texts: dict[str, str] = {}
        self._periods: dict[str, int] = {}  # 0 なら流さない (収まっている)
        self._offsets: dict[str, float] = {}
        self._text_x = 0
        self._text_y: dict[str, int] = {"title": 0, "artist": 0}
        self._scroll_at = 0.0  # この時刻から流し始める
        self._scrolled_at = time.monotonic()

        self.win = tk.Toplevel(root)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)

        icon_set = IconSet(root)
        self.icons = icon_set
        text_family = pick_font(root, TEXT_FAMILIES, "Segoe UI")
        self._text_family = text_family
        self._icon_family = icon_set.family
        self.f_title = tkfont.Font(root=root, family=text_family, size=-12, weight="bold")
        self.f_artist = tkfont.Font(root=root, family=text_family, size=-11)
        self.f_icon = tkfont.Font(root=root, family=icon_set.family, size=-14)

        self.colors = self._colors(None)
        self.canvas = tk.Canvas(
            self.win, width=10, height=10, highlightthickness=0, bd=0,
            bg=self.colors["bg"],
        )
        self.canvas.pack()
        self._bind()
        self._built_size: Optional[tuple[int, int]] = None
        self._anchor_used: Optional[str] = None

    # ------------------------------------------------------------------ 配色

    def _colors(self, sampled: Optional[str]) -> dict[str, str]:
        """タスクバーの地色から、なじむ文字色を決める。"""
        bg = self.config.bar_bg or sampled or "#1f1f22"
        dark = luminance(bg) < 0.45
        if dark:
            colors = {
                "bg": bg,
                "title": "#ffffff",
                "artist": "#bdbdc6",
                "button": "#e4e4ea",
                "hover": "#ffffff",
                "disabled": "#6a6a72",
                "track": "#55555f",
                "end": "#9a9aa4",
            }
            # 流れる文字の上に置くので、地色から少し浮かせた座を敷く
            colors["chip"] = mix(bg, "#ffffff", 0.16)
            colors["chip_hover"] = mix(bg, "#ffffff", 0.30)
            return colors
        colors = {
            "bg": bg,
            "title": "#101014",
            "artist": "#4a4a52",
            "button": "#26262c",
            "hover": "#000000",
            "disabled": "#a0a0a8",
            "track": "#b6b6be",
            "end": "#7a7a84",
        }
        colors["chip"] = mix(bg, "#000000", 0.12)
        colors["chip_hover"] = mix(bg, "#000000", 0.22)
        return colors

    def _sample_background(self, x: int, y: int, width: int, height: int) -> None:
        """バーを置く場所のタスクバー色を拾って、地色を合わせる。"""
        if self.config.bar_bg:
            return
        cy = y + height // 2
        points = [(x + width * i // 6, cy) for i in range(1, 6)]
        points += [(x + width * i // 6, y + height // 4) for i in range(1, 6)]
        color = winapi.sample_color(points)
        if color:
            self.colors = self._colors(color)

    # ------------------------------------------------------------------ 組み立て

    def _build(self, width: int, height: int) -> None:
        """バーの中身を作り直す (幅・高さが変わったときだけ)。"""
        c = self.canvas
        c.delete("all")
        c.configure(width=width, height=height, bg=self.colors["bg"])
        self.win.configure(bg=self.colors["bg"])

        pad = max(4, round(height * 0.12))
        art = height - pad * 2
        text_x = pad + art + max(6, round(height * 0.16))

        btn = max(20, round(height * 0.60))
        icon_px = max(11, round(height * 0.26))
        self.f_icon.configure(size=-icon_px)
        self.f_title.configure(size=-max(11, round(height * 0.27)))
        self.f_artist.configure(size=-max(10, round(height * 0.22)))

        right = width - pad
        next_cx = right - btn // 2
        play_cx = next_cx - btn
        prev_cx = play_cx - btn
        cy = height // 2

        chip_h = max(18, round(height * 0.56))
        chip_w = max(18, btn - max(2, round(height * 0.06)))
        chips_left = prev_cx - chip_w // 2

        # 文字はバーの右端まで使う (収まらない分は流れてボタンの裏を通る)。
        # ただし「流すかどうか」はボタンの手前までで判定する。ここに収まって
        # いれば動かさずに全部読めるし、はみ出すなら流さないと末尾がボタンの
        # 裏に隠れたままになる。
        self._text_x = text_x
        self._text_width = max(20, chips_left - max(4, round(height * 0.08)) - text_x)
        title_y = round(height * 0.06)
        artist_y = round(height * 0.58)
        self._text_y = {"title": title_y, "artist": artist_y}

        # 1. 文字 (いちばん奥)
        self.title_item = c.create_text(
            text_x, title_y, anchor="nw",
            font=self.f_title, fill=self.colors["title"],
        )
        self.artist_item = c.create_text(
            text_x, artist_y, anchor="nw",
            font=self.f_artist, fill=self.colors["artist"],
        )

        # 2. アルバム アートの領域を隠す覆い (流れてきた文字をここで消す)
        c.create_rectangle(0, 0, text_x - 1, height, fill=self.colors["bg"], width=0)
        self.art_item = c.create_image(pad, pad, anchor="nw")

        # 3. シーク バーはタイトルとアーティストの間。ボタンの手前で止める
        prog_y = round(height * 0.50)
        prog_right = prev_cx - btn // 2 - max(6, round(height * 0.12))
        self.track_item = c.create_line(
            text_x, prog_y, prog_right, prog_y,
            width=2, fill=self.colors["track"], capstyle=tk.ROUND,
        )
        self.fill_item = c.create_line(
            text_x, prog_y, text_x, prog_y,
            width=2, fill=self.colors["title"], capstyle=tk.ROUND, state="hidden",
        )
        # 終端が分かるように、右端に縦の目印を立てる
        end_h = max(3, round(height * 0.10))
        self.end_item = c.create_line(
            prog_right, prog_y - end_h, prog_right, prog_y + end_h,
            width=2, fill=self.colors["end"], capstyle=tk.ROUND,
        )
        self._prog = (text_x, prog_right, prog_y)

        # 4. ボタン (座 + 記号)。文字より前面に出したいので最後に作る
        radius = max(3, round(min(chip_w, chip_h) * 0.30))
        self.chips = {}
        self.buttons = {}
        for name, cx_ in (("prev", prev_cx), ("play", play_cx), ("next", next_cx)):
            self.chips[name] = self._rounded(
                cx_ - chip_w // 2, cy - chip_h // 2,
                cx_ + chip_w // 2, cy + chip_h // 2, radius,
            )
            self.buttons[name] = c.create_text(
                cx_, cy, font=self.f_icon, fill=self.colors["button"],
                text=self.icons.glyph("play" if name == "play" else name),
            )
        self.prev_item = self.buttons["prev"]
        self.play_item = self.buttons["play"]
        self.next_item = self.buttons["next"]

        half = chip_w // 2
        self._hitboxes = {
            "prev": (prev_cx - half, 0, prev_cx + half, height),
            "play": (play_cx - half, 0, play_cx + half, height),
            "next": (next_cx - half, 0, next_cx + half, height),
        }
        self._art_size = art
        self._art_key = None  # 作り直したので再描画させる
        self._texts = {}  # 作り直したので文字も入れ直す
        self._built_size = (width, height)

    def _rounded(self, x0: int, y0: int, x1: int, y1: int, r: int):
        """角丸の四角。Tk の Canvas には無いので、滑らかな多角形で描く。"""
        points = [
            x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r,
            x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1,
            x0, y1, x0, y1 - r, x0, y0 + r, x0, y0,
        ]
        return self.canvas.create_polygon(
            points, smooth=True, splinesteps=12, width=0, fill=self.colors["chip"]
        )

    def _bind(self) -> None:
        c = self.canvas
        c.bind("<Motion>", lambda e: self._set_hover(self._hit(e.x, e.y)))
        c.bind("<Leave>", lambda _e: self._set_hover(None))
        c.bind("<Button-1>", self._on_press)
        c.bind("<ButtonRelease-1>", self._on_release)
        c.bind("<Button-3>", lambda e: self.on_context_menu(e.x_root, e.y_root))

    # ------------------------------------------------------------------ 入力

    def _hit(self, x: int, y: int) -> Optional[str]:
        for name in ("prev", "play", "next"):
            box = self._hitboxes.get(name)
            if box and box[0] <= x <= box[2] and box[1] <= y <= box[3]:
                return name if self._enabled(name) else None
        return None

    def _enabled(self, name: str) -> bool:
        st = self.state
        return {
            "prev": st.can_prev,
            "next": st.can_next,
            "play": st.can_play or st.can_pause,
        }.get(name, True)

    def _set_hover(self, name: Optional[str]) -> None:
        if name == self._hover:
            return
        self._hover = name
        self.canvas.configure(cursor="hand2" if name else "")
        self._paint_buttons()

    def _on_press(self, event) -> None:
        self._pressed = self._hit(event.x, event.y)

    def _on_release(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name is None or name != self._pressed:
            return
        if name == "prev":
            self.controller.previous_track()
        elif name == "next":
            self.controller.next_track()
        elif name == "play":
            self.controller.toggle_play_pause()

    # ------------------------------------------------------------------ 位置合わせ

    def _placement(self, rect: tuple[int, int, int, int]):
        """設定に応じた (x, 幅) を返す。置ける場所がなければ None。"""
        left, _top, right, _bottom = rect
        notify = winapi.child_rect("TrayNotifyWnd")
        tray_left = notify[0] if notify else right - round(200 * self.scale)
        start = winapi.child_rect("Start")
        start_left = start[0] if start else left

        gap = round((self.config.bar_gap or DEFAULT_GAP) * self.scale)
        width = round((self.config.bar_width or DEFAULT_WIDTH) * self.scale)
        anchor = self.config.bar_anchor

        if anchor == "start" and start_left - left > width:
            # スタート ボタンのすぐ左に寄せる (アイコンが中央寄せのとき)
            region_right, align_right = start_left, True
        elif anchor == "left":
            # 左端。スタート ボタンが中央寄せなら、その手前までに収める
            region_right = start_left if start_left - left >= width + gap * 2 else tray_left
            align_right = False
        else:
            region_right, align_right = tray_left, True

        width = min(width, region_right - left - gap * 2)
        if width < round(MIN_WIDTH * self.scale):
            return None
        return (region_right - gap - width if align_right else left + gap), width

    def sync(self) -> None:
        """タスクバーの位置・高さに追従し、重なり順を保つ。"""
        # ラベルどおり「再生中」だけを見る。has_media だと一時停止中のセッションが
        # 残っているアプリ (ブラウザーなど) で永久に隠れず、設定が効かなかった。
        if self.config.bar_hide_when_idle and not self.state.is_playing:
            self._hide()
            return
        rect = winapi.taskbar_rect()
        if rect is None or winapi.foreground_is_fullscreen():
            self._hide()
            return

        height = rect[3] - rect[1]
        placement = self._placement(rect)
        if placement is None or height <= 0:
            self._hide()
            return
        x, width = placement
        y = rect[1]

        # 置き場所を変えたときは、その場所の地色を測り直してから作り直す
        anchor = self.config.bar_anchor
        if self._built_size != (width, height) or self._anchor_used != anchor:
            if self.visible:
                self.win.withdraw()
                self.visible = False
                self.win.update_idletasks()
                time.sleep(0.05)  # 画面から消えるのを待ってから色を拾う
            self._sample_background(x, y, width, height)
            self._anchor_used = anchor
            self._build(width, height)
            self.update_state(self.state)

        # 位置は Tk 側に持たせ、SetWindowPos は重なり順の維持だけに使う
        # (両方から動かすと、Tk の再配置に負けて元の位置に戻ってしまう)
        geometry = (x, y, width, height)
        if self._geometry != geometry:
            self.win.geometry(f"{width}x{height}+{x}+{y}")
            self._geometry = geometry

        if not self.visible:
            self.win.deiconify()
            self.win.update_idletasks()
            self._hwnd = winapi.toplevel_hwnd(self.win.winfo_id())
            winapi.make_tool_window(self._hwnd)
            self.visible = True

        self._keep_in_front(x + width // 2, y + height // 2)

    def _keep_in_front(self, cx: int, cy: int) -> None:
        """タスクバーに潜られたら前に出し直す。

        タスクバーをクリックするとタスクバー自身が持ち上がるので、覆われていたら
        並べ直す。ふつうはこれで次の tick には戻る。

        ただし スタート メニューを開くと、タスクバーは z バンドごと上がり
        (1 → 6)、**スタート メニューを閉じただけでは下りてこない**。別の
        ウィンドウが活性化されるまで上がったままで、その間はこちらから
        どうやっても前に出られない (UIAccess 権限が要る)。越えられないと
        分かっている間は要求を出さない。
        """
        if not winapi.is_covered(self._hwnd, cx, cy):
            if DEBUG and self._covered_since is not None:
                print(f"復帰 ({time.monotonic() - self._covered_since:.1f}s)", flush=True)
            self._covered_since = None
            return

        if self._covered_since is None:
            self._covered_since = time.monotonic()
            if DEBUG:
                print("タスクバーに覆われました", flush=True)

        taskbar = winapi.taskbar()
        their_band = winapi.window_band(taskbar) if taskbar else None
        our_band = winapi.window_band(self._hwnd)
        if their_band is not None and our_band is not None and their_band > our_band:
            if DEBUG:
                print(f"  バンドが上 ({our_band} < {their_band}) なので手が出せない", flush=True)
            return
        winapi.raise_to_top(self._hwnd)

    def _hide(self) -> None:
        if self.visible:
            self.win.withdraw()
            self.visible = False
            self._set_hover(None)

    # ------------------------------------------------------------------ 描画

    def update_state(self, state: NowPlaying) -> None:
        self.state = state
        if self._built_size is None:
            return
        c = self.canvas

        has_media = state.has_media and bool(state.title or state.artist)
        title = state.title if has_media else EMPTY_TITLE
        artist = (
            " · ".join(x for x in (state.artist, state.album, state.app_name) if x)
            if has_media
            else ""
        )
        self._set_text("title", self.title_item, self.f_title, title)
        self._set_text("artist", self.artist_item, self.f_artist, artist)

        art_key = f"{state.art_key}|{state.status}|{self._art_size}"
        if art_key != self._art_key:
            self._art_key = art_key
            image = icons.album_art(
                state.thumbnail,
                self._art_size,
                max(2, self._art_size // 8),
                self.colors["artist"],
                self.colors["track"],
            )
            if has_media and not state.is_playing:
                image = icons.dim(image, 0.35)
            self._art_image = ImageTk.PhotoImage(icons.flatten(image, self.colors["bg"]))
            c.itemconfigure(self.art_item, image=self._art_image)

        c.itemconfigure(
            self.play_item, text=self.icons.glyph("pause" if state.is_playing else "play")
        )
        self._paint_buttons()
        self.update_progress()

    def _set_text(self, key: str, item, font, text: str) -> None:
        """文字を入れる。入りきらないときは流せるように 2 つ繋げて持たせる。

        同じ文字なら位置を保つ (更新のたびに頭へ戻ると読めなくなる)。
        """
        if self._texts.get(key) == text:
            return
        self._texts[key] = text
        if font.measure(text) <= self._text_width:
            self._periods[key] = 0
            self.canvas.itemconfigure(item, text=text)
        else:
            # 末尾まで流れたら頭に戻る、を繋ぎ目なく見せるため 2 回並べる
            self._periods[key] = font.measure(text + SCROLL_GAP)
            self.canvas.itemconfigure(item, text=text + SCROLL_GAP + text)
        self._offsets[key] = 0.0
        self.canvas.coords(item, self._text_x, self._text_y[key])
        self._scroll_at = time.monotonic() + SCROLL_DELAY

    def scroll_tick(self) -> None:
        """流れる文字を 1 コマ進める。app 側から短い間隔で呼ばれる。"""
        if not self.visible or self._built_size is None:
            return
        now = time.monotonic()
        elapsed, self._scrolled_at = now - self._scrolled_at, now
        if now < self._scroll_at or elapsed <= 0 or elapsed > 1.0:
            return
        step = SCROLL_SPEED * self.scale * elapsed
        for key, item in (("title", self.title_item), ("artist", self.artist_item)):
            period = self._periods.get(key, 0)
            if period <= 0:
                continue
            offset = self._offsets.get(key, 0.0) + step
            if offset >= period:
                offset -= period
            self._offsets[key] = offset
            try:
                self.canvas.coords(item, self._text_x - offset, self._text_y[key])
            except tk.TclError:
                return

    def update_progress(self) -> None:
        if self._built_size is None:
            return
        c = self.canvas
        state = self.state
        x0, x1, y = self._prog
        shown = "normal" if state.has_media else "hidden"
        c.itemconfigure(self.track_item, state=shown)
        c.itemconfigure(self.end_item, state=shown)
        if not state.has_media or state.duration <= 0:
            c.itemconfigure(self.fill_item, state="hidden")
            return
        ratio = min(1.0, max(0.0, state.live_position() / state.duration))
        c.itemconfigure(self.fill_item, state="normal")
        c.coords(self.fill_item, x0, y, max(x0 + 1, x0 + (x1 - x0) * ratio), y)

    def _paint_buttons(self) -> None:
        if self._built_size is None:
            return
        c = self.canvas
        for name, item in self.buttons.items():
            hover = self._hover == name
            if not self._enabled(name):
                color = self.colors["disabled"]
            elif hover:
                color = self.colors["hover"]
            else:
                color = self.colors["button"]
            c.itemconfigure(item, fill=color)
            c.itemconfigure(
                self.chips[name],
                fill=self.colors["chip_hover"] if hover else self.colors["chip"],
            )
