"""タスクバーの通知領域 (トレイ) のアイコンとメニュー。"""

from __future__ import annotations

import threading
from typing import Callable, Optional

import pystray
from pystray import Menu, MenuItem

import icons
from art_popup import POPUP_HEIGHTS
from media_session import NowPlaying
from taskbar_bar import BAR_ANCHORS, BAR_WIDTHS
from theme import Palette

ICON_SIZE = 64
TOOLTIP_LIMIT = 120  # Windows のツールチップは 127 文字まで


class Tray:
    """曲情報を映すトレイ アイコン。操作はコールバックで通知する。"""

    def __init__(
        self,
        state_provider: Callable[[], NowPlaying],
        on_play_pause: Callable[[], None],
        on_next: Callable[[], None],
        on_prev: Callable[[], None],
        on_toggle_hide_when_idle: Callable[[], None],
        on_toggle_wheel_volume: Callable[[], None],
        on_set_anchor: Callable[[str], None],
        on_set_width: Callable[[int], None],
        on_set_art_size: Callable[[int], None],
        on_select_session: Callable[[Optional[str]], None],
        on_quit: Callable[[], None],
        config,
    ) -> None:
        self._state = state_provider
        self._on_select_session = on_select_session
        self._on_set_anchor = on_set_anchor
        self._on_set_width = on_set_width
        self._on_set_art_size = on_set_art_size
        self.config = config
        self.palette = Palette.current()

        self._menu_key: Optional[tuple] = None
        self._thread: Optional[threading.Thread] = None

        menu = Menu(
            MenuItem("再生 / 一時停止", lambda: on_play_pause(), default=True),
            MenuItem("次の曲", lambda: on_next()),
            MenuItem("前の曲", lambda: on_prev()),
            Menu.SEPARATOR,
            MenuItem("再生元", Menu(self._session_items)),
            MenuItem("表示位置", Menu(self._anchor_items)),
            MenuItem("バーの幅", Menu(self._width_items)),
            MenuItem("アルバム アートの大きさ", Menu(self._art_size_items)),
            Menu.SEPARATOR,
            MenuItem(
                "再生中のときだけ表示",
                lambda: on_toggle_hide_when_idle(),
                checked=lambda _i: self.config.bar_hide_when_idle,
            ),
            MenuItem(
                "ホイールで音量を調整",
                lambda: on_toggle_wheel_volume(),
                checked=lambda _i: self.config.bar_wheel_volume,
            ),
            Menu.SEPARATOR,
            MenuItem("終了", lambda: on_quit()),
        )

        self.icon = pystray.Icon(
            "MuuTask",
            icon=self._make_image(),
            title="MuuTask",
            menu=menu,
        )

    # ------------------------------------------------------------------ メニュー

    def _session_items(self):
        # pystray はコールバックの引数の数で呼び出し方を決めるため、
        # 既定値ではなくクロージャで app_id を束縛する。
        def select(app_id: Optional[str]):
            return lambda _icon: self._on_select_session(app_id)

        def is_selected(app_id: Optional[str]):
            return lambda _item: self.config.session == app_id

        yield MenuItem(
            "自動 (再生中のアプリ)",
            select(None),
            checked=is_selected(None),
            radio=True,
        )
        for app_id, name in self._state().sessions:
            yield MenuItem(name, select(app_id), checked=is_selected(app_id), radio=True)

    def _anchor_items(self):
        def select(value: str):
            return lambda _icon: self._on_set_anchor(value)

        def is_selected(value: str):
            return lambda _item: self.config.bar_anchor == value

        for value, label in BAR_ANCHORS:
            yield MenuItem(label, select(value), checked=is_selected(value), radio=True)

    def _width_items(self):
        def select(value: int):
            return lambda _icon: self._on_set_width(value)

        def is_selected(value: int):
            return lambda _item: self.config.bar_width == value

        for value, label in BAR_WIDTHS:
            yield MenuItem(
                f"{label} ({value})", select(value), checked=is_selected(value), radio=True
            )

    def _art_size_items(self):
        def select(value: int):
            return lambda _icon: self._on_set_art_size(value)

        def is_selected(value: int):
            return lambda _item: self.config.popup_height == value

        for value, label in POPUP_HEIGHTS:
            yield MenuItem(
                f"{label} ({value})", select(value), checked=is_selected(value), radio=True
            )

    # ------------------------------------------------------------------ 見た目

    def _make_image(self):
        """トレイのアイコン。曲が変わっても音符のままにしておく。

        通知領域では 16x16 まで縮められるので、アルバム アートを入れても
        潰れて何が写っているか分からない。ここは MuuTask の目印として
        音符を出し続け、いま何が鳴っているかはツールチップに任せる。
        """
        return icons.note_icon(ICON_SIZE, self.palette.accent, self.palette.card, 10)

    @staticmethod
    def _tooltip(state: NowPlaying) -> str:
        if not state.has_media or not (state.title or state.artist):
            return "MuuTask — 再生中の音楽はありません"
        mark = "▶" if state.is_playing else "❚❚"
        parts = [p for p in (state.title, state.artist) if p]
        text = f"{mark} " + " — ".join(parts)
        if state.app_name:
            text += f"  ({state.app_name})"
        return text[:TOOLTIP_LIMIT]

    def update(self, state: NowPlaying) -> None:
        """再生状態に合わせてツールチップとメニューを更新する。"""
        tooltip = self._tooltip(state)
        if self.icon.title != tooltip:
            self.icon.title = tooltip

        menu_key = (
            state.sessions,
            self.config.session,
            self.config.bar_hide_when_idle,
            self.config.bar_wheel_volume,
            self.config.bar_anchor,
            self.config.bar_width,
            self.config.popup_height,
        )
        if menu_key != self._menu_key:
            self._menu_key = menu_key
            self.refresh_menu()

    def refresh_menu(self) -> None:
        try:
            self.icon.update_menu()
        except Exception:
            pass

    # ------------------------------------------------------------------ 開始/終了

    def start(self) -> None:
        self._thread = threading.Thread(
            target=self.icon.run, name="muutask-tray", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        try:
            self.icon.stop()
        except Exception:
            pass
