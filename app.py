"""MuuTask — いま再生中の曲をタスクバー (通知領域) に表示するミニ プレーヤー。

トレイのアイコンにアルバム アートと曲名が出て、クリックで開くパネルから
再生 / 一時停止・曲送り・シークができる。

    pythonw app.py       (コンソールなしで常駐)
    python  app.py       (ログを見たいとき)
"""

from __future__ import annotations

import ctypes
import queue
import sys
import tkinter as tk
from ctypes import wintypes
from typing import Callable

import theme
from config import Config
from media_session import MediaController, NowPlaying
from taskbar_bar import BAR_ANCHORS, BAR_WIDTHS, TaskbarBar
from tray_icon import Tray

MUTEX_NAME = "MuuTask.SingleInstance"
DRAIN_INTERVAL = 80  # ms
TICK_INTERVAL = 250  # ms
SYNC_INTERVAL = 250  # ms (タスクバーへの追従。長いと潜ったときの復帰が目立つ)


def _already_running() -> bool:
    """二重起動を防ぐ (名前付きミューテックス)。

    ミューテックスのハンドルはプロセス終了まで開いたままにしておく。
    """
    try:
        # ctypes は呼び出しごとにエラー コードを退避するので use_last_error が要る
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateMutexW.restype = wintypes.HANDLE
        kernel32.CreateMutexW.argtypes = [
            wintypes.LPVOID,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        ]
        handle = kernel32.CreateMutexW(None, False, MUTEX_NAME)
        if not handle:
            return False
        globals()["_MUTEX_HANDLE"] = handle
        return ctypes.get_last_error() == 183  # ERROR_ALREADY_EXISTS
    except (AttributeError, OSError):
        return False


class App:
    def __init__(self) -> None:
        theme.enable_dpi_awareness()

        self.config = Config.load()
        self.state = NowPlaying()
        self._events: "queue.Queue[Callable[[], None]]" = queue.Queue()
        self._pending_state: NowPlaying | None = None

        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title("MuuTask")

        self.controller = MediaController(self._on_state)
        self.bar = TaskbarBar(
            self.root,
            self.controller,
            self.config,
            scale=theme.ui_scale(),
            on_context_menu=self._show_menu,
        )
        self.menu = self._build_menu()
        self.tray = Tray(
            state_provider=lambda: self.state,
            on_play_pause=self.controller.toggle_play_pause,
            on_next=self.controller.next_track,
            on_prev=self.controller.previous_track,
            on_toggle_hide_when_idle=lambda: self._post(self._toggle_hide_when_idle),
            on_set_anchor=lambda a: self._post(lambda: self._set_anchor(a)),
            on_set_width=lambda w: self._post(lambda: self._set_width(w)),
            on_select_session=lambda app_id: self._post(
                lambda: self._select_session(app_id)
            ),
            on_quit=lambda: self._post(self.quit),
            config=self.config,
        )

    # ------------------------------------------------------------------ スレッド間

    def _post(self, func: Callable[[], None]) -> None:
        """別スレッドからの処理を Tk のメインループに渡す。"""
        self._events.put(func)

    def _on_state(self, state: NowPlaying) -> None:
        # ワーカー スレッドから呼ばれる。最新の 1 件だけ持てばよい。
        self._pending_state = state

    def _drain(self) -> None:
        while True:
            try:
                self._events.get_nowait()()
            except queue.Empty:
                break
            except Exception:
                pass

        state = self._pending_state
        if state is not None and state is not self.state:
            self.state = state
            self.bar.update_state(state)
            self.tray.update(state)

        self.root.after(DRAIN_INTERVAL, self._drain)

    def _tick(self) -> None:
        if self.state.is_playing and self.bar.visible:
            self.bar.update_progress()
        self.root.after(TICK_INTERVAL, self._tick)

    def _sync(self) -> None:
        try:
            self.bar.sync()
        except tk.TclError:
            pass
        self.root.after(SYNC_INTERVAL, self._sync)

    # ------------------------------------------------------------------ メニュー

    def _var(self, value: bool) -> tk.IntVar:
        """メニューのチェック状態用。参照を持っていないと GC されてしまう。"""
        var = tk.IntVar(master=self.root, value=int(value))
        self._menu_vars.append(var)
        return var

    def _build_menu(self) -> tk.Menu:
        """バーの右クリック メニュー。開くたびに中身を作り直す。"""
        self._menu_vars: list[tk.Variable] = []
        menu = tk.Menu(self.root, tearoff=0)
        menu.add_command(label="再生 / 一時停止", command=self.controller.toggle_play_pause)
        menu.add_command(label="次の曲", command=self.controller.next_track)
        menu.add_command(label="前の曲", command=self.controller.previous_track)
        menu.add_separator()

        sources = tk.Menu(menu, tearoff=0)
        current = self.config.session
        choice = tk.StringVar(master=self.root, value=current or "")
        self._menu_vars.append(choice)
        sources.add_radiobutton(
            label="自動 (再生中のアプリ)",
            value="",
            variable=choice,
            command=lambda: self._select_session(None),
        )
        for app_id, name in self.state.sessions:
            sources.add_radiobutton(
                label=name,
                value=app_id,
                variable=choice,
                command=lambda a=app_id: self._select_session(a),
            )
        menu.add_cascade(label="再生元", menu=sources)
        menu.add_separator()

        positions = tk.Menu(menu, tearoff=0)
        anchor = tk.StringVar(master=self.root, value=self.config.bar_anchor)
        self._menu_vars.append(anchor)
        for value, label in BAR_ANCHORS:
            positions.add_radiobutton(
                label=label,
                value=value,
                variable=anchor,
                command=lambda v=value: self._set_anchor(v),
            )
        menu.add_cascade(label="表示位置", menu=positions)

        widths = tk.Menu(menu, tearoff=0)
        width = tk.IntVar(master=self.root, value=self.config.bar_width)
        self._menu_vars.append(width)
        for value, label in BAR_WIDTHS:
            widths.add_radiobutton(
                label=f"{label} ({value})",
                value=value,
                variable=width,
                command=lambda v=value: self._set_width(v),
            )
        menu.add_cascade(label="バーの幅", menu=widths)

        menu.add_checkbutton(
            label="再生中のときだけ表示",
            variable=self._var(self.config.bar_hide_when_idle),
            command=self._toggle_hide_when_idle,
        )
        menu.add_separator()
        menu.add_command(label="終了", command=self.quit)
        return menu

    def _show_menu(self, x: int, y: int) -> None:
        self.menu = self._build_menu()
        try:
            self.menu.tk_popup(x, y)
        finally:
            self.menu.grab_release()

    # ------------------------------------------------------------------ 操作

    def _set_anchor(self, anchor: str) -> None:
        self.config.bar_anchor = anchor
        self.config.save()
        self.bar.sync()
        self.tray.refresh_menu()

    def _set_width(self, width: int) -> None:
        self.config.bar_width = width
        self.config.save()
        self.bar.sync()
        self.tray.refresh_menu()

    def _toggle_hide_when_idle(self) -> None:
        self.config.bar_hide_when_idle = not self.config.bar_hide_when_idle
        self.config.save()
        self.bar.sync()
        self.tray.refresh_menu()

    def _select_session(self, app_id) -> None:
        self.config.session = app_id
        self.config.save()
        self.controller.select_session(app_id)
        self.tray.refresh_menu()

    def quit(self) -> None:
        self.controller.stop()
        self.tray.stop()
        try:
            self.root.destroy()
        except tk.TclError:
            pass

    # ------------------------------------------------------------------ 実行

    def run(self) -> None:
        if self.config.session:
            self.controller.select_session(self.config.session)
        self.controller.start()
        self.tray.start()
        self.root.after(DRAIN_INTERVAL, self._drain)
        self.root.after(TICK_INTERVAL, self._tick)
        self.root.after(0, self._sync)
        try:
            self.root.mainloop()
        finally:
            self.controller.stop()
            self.tray.stop()


def main() -> int:
    if _already_running():
        return 0
    App().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
