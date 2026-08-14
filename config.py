"""設定の保存と、スタートアップ登録。"""

from __future__ import annotations

import json
import os
import sys
import winreg
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

APP_NAME = "MuuTask"
APP_VERSION = "1.0.0"
CONFIG_DIR = Path(os.environ.get("APPDATA", Path.home())) / APP_NAME
CONFIG_PATH = CONFIG_DIR / "config.json"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


@dataclass
class Config:
    pinned: bool = False
    x: Optional[int] = None
    y: Optional[int] = None
    session: Optional[str] = None  # 固定するアプリ ID (None = 自動)

    # タスクバーに重ねて出すバー
    bar_anchor: str = "tray"  # left / start / tray (taskbar_bar.BAR_ANCHORS)
    bar_hide_when_idle: bool = False  # 再生中のときだけ出す
    bar_width: int = 340  # 96 DPI 基準の px
    bar_gap: int = 8  # 通知領域との間隔
    bar_bg: Optional[str] = None  # "#rrggbb" 固定したいとき (既定は自動判定)

    @classmethod
    def load(cls) -> "Config":
        try:
            # utf-8-sig: 手で編集したときの BOM 付きファイルも読めるように
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            return cls()
        known = {f for f in cls.__dataclass_fields__}
        return cls(**{k: v for k, v in data.items() if k in known})

    def save(self) -> None:
        try:
            CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            CONFIG_PATH.write_text(
                json.dumps(asdict(self), indent=2), encoding="utf-8"
            )
        except OSError:
            pass


# ----------------------------------------------------------------- スタートアップ


def _launch_command() -> str:
    """ログオン時に実行するコマンド (コンソールなしで起動)。"""
    if getattr(sys, "frozen", False):
        # exe 版。app.py は exe の中なので実行ファイルだけを登録する
        return f'"{Path(sys.executable)}"'
    exe = Path(sys.executable)
    pythonw = exe.with_name("pythonw.exe")
    launcher = pythonw if pythonw.exists() else exe
    script = Path(__file__).with_name("app.py")
    return f'"{launcher}" "{script}"'


def autostart_enabled() -> bool:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            value, _ = winreg.QueryValueEx(key, APP_NAME)
            return bool(value)
    except OSError:
        return False


def set_autostart(enabled: bool) -> None:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_WRITE) as key:
            if enabled:
                winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, _launch_command())
            else:
                try:
                    winreg.DeleteValue(key, APP_NAME)
                except FileNotFoundError:
                    pass
    except OSError:
        pass
