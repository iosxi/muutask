"""タスクバーの通知領域 (トレイ) のアイコンとメニュー。"""

from __future__ import annotations

import threading
from typing import Callable, Optional

import pystray
from pystray import Menu, MenuItem

import config as config_module
import icons
from media_session import NowPlaying
from taskbar_bar import BAR_ANCHORS
from theme import Palette

ICON_SIZE = 64
TOOLTIP_LIMIT = 120  # Windows のツールチップは 127 文字まで


class Tray:
    """曲情報を映すトレイ アイコン。操作はコールバックで通知する。"""

    def __init__(
        self,
        state_provider: Callable[[], NowPlaying],
        on_toggle_panel: Callable[[], None],
        on_play_pause: Callable[[], None],
        on_next: Callable[[], None],
        on_prev: Callable[[], None],
        on_toggle_pin: Callable[[], None],
        on_toggle_hide_when_idle: Callable[[], None],
        on_set_anchor: Callable[[str], None],
        on_select_session: Callable[[Optional[str]], None],
        on_quit: Callable[[], None],
        config,
    ) -> None:
        self._state = state_provider
        self._on_select_session = on_select_session
        self._on_set_anchor = on_set_anchor
        self.config = config
        self.palette = Palette.current()

        self._icon_key: Optional[str] = None
        self._menu_key: Optional[tuple] = None
        self._thread: Optional[threading.Thread] = None

        menu = Menu(
            MenuItem("パネルを開く / 閉じる", lambda: on_toggle_panel(), default=True),
            Menu.SEPARATOR,
            MenuItem("再生 / 一時停止", lambda: on_play_pause()),
            MenuItem("次の曲", lambda: on_next()),
            MenuItem("前の曲", lambda: on_prev()),
            Menu.SEPARATOR,
            MenuItem("再生元", Menu(self._session_items)),
            MenuItem("表示位置", Menu(self._anchor_items)),
            Menu.SEPARATOR,
            MenuItem(
                "再生中のときだけ表示",
                lambda: on_toggle_hide_when_idle(),
                checked=lambda _i: self.config.bar_hide_when_idle,
            ),
            MenuItem(
                "詳細パネルを常に表示",
                lambda: on_toggle_pin(),
                checked=lambda _i: self.config.pinned,
            ),
            MenuItem(
                "Windows 起動時に実行",
                self._toggle_autostart,
                checked=lambda _i: config_module.autostart_enabled(),
            ),
            Menu.SEPARATOR,
            MenuItem("終了", lambda: on_quit()),
        )

        self.icon = pystray.Icon(
            "MuuTask",
            icon=self._make_image(NowPlaying()),
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

    def _toggle_autostart(self, _icon=None, _item=None) -> None:
        config_module.set_autostart(not config_module.autostart_enabled())
        self.refresh_menu()

    # ------------------------------------------------------------------ 見た目

    def _make_image(self, state: NowPlaying):
        image = icons.album_art(
            state.thumbnail, ICON_SIZE, 10, self.palette.accent, self.palette.card
        )
        if state.has_media and not state.is_playing:
            image = icons.dim(image, 0.35)
        return image

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
        """再生状態に合わせてアイコンとツールチップを更新する。"""
        key = f"{state.track_key}|{state.status}"
        if key != self._icon_key:
            self._icon_key = key
            try:
                self.icon.icon = self._make_image(state)
            except Exception:
                pass
        tooltip = self._tooltip(state)
        if self.icon.title != tooltip:
            self.icon.title = tooltip

        menu_key = (
            state.sessions,
            self.config.session,
            self.config.pinned,
            self.config.bar_hide_when_idle,
            self.config.bar_anchor,
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
