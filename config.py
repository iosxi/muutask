"""設定の保存。

レジストリには一切書かない。設定は exe と同じフォルダーの config.json だけ
なので、やめるときはフォルダーごと削除すれば痕跡が残らない。
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

APP_NAME = "MuuTask"
APP_VERSION = "9"  # v1 から配布のたびに 1 ずつ上げる


def _config_path() -> Path:
    """設定は exe と同じフォルダーに置く。

    アンインストーラーの無いツールなので、消し忘れる場所を作らないことを
    優先している。onefile では sys._MEIPASS が %TEMP% の展開先を指すので、
    そちらではなく sys.executable のある階層を使う (終了時に消える場所に
    書かないため)。
    """
    base = (
        Path(sys.executable).parent
        if getattr(sys, "frozen", False)
        else Path(__file__).resolve().parent
    )
    return base / "config.json"


CONFIG_PATH = _config_path()


def _legacy_config_path() -> Optional[Path]:
    """v1 以前の保存先。%APPDATA% が無い環境では None を返す。"""
    base = os.environ.get("APPDATA")
    return Path(base) / APP_NAME / "config.json" if base else None


def _take_over_legacy_config() -> None:
    """v1 以前が %APPDATA%\\MuuTask\\ に残した設定を引き継いで、その跡を消す。

    設定を exe と同じフォルダーへ移した目的が「やめるときにフォルダーごと
    消せば痕跡が残らないこと」なので、旧フォルダーを放置すると目的を
    果たせない。新しい場所に設定がまだ無いときだけ中身を移し、旧ファイルを
    消してからフォルダーも消す。rmdir は空でなければ失敗するので、他人の
    ものは残る。触るのは自分で作った %APPDATA%\\MuuTask\\ だけ。
    """
    old = _legacy_config_path()
    if old is None or not old.exists():
        return
    try:
        if not CONFIG_PATH.exists():
            CONFIG_PATH.write_bytes(old.read_bytes())
        old.unlink()
        old.parent.rmdir()
    except OSError:
        pass


@dataclass
class Config:
    session: Optional[str] = None  # 固定するアプリ ID (None = 自動)

    # タスクバーに重ねて出すバー
    bar_anchor: str = "tray"  # left / start / tray (taskbar_bar.BAR_ANCHORS)
    bar_hide_when_idle: bool = False  # 再生中のときだけ出す
    bar_width: int = 340  # 96 DPI 基準の px
    bar_gap: int = 8  # 通知領域との間隔
    bar_bg: Optional[str] = None  # "#rrggbb" 固定したいとき (既定は自動判定)
    popup_height: int = 180  # ホバーで出るアルバム アートの一辺 (96 DPI 基準の px)

    @classmethod
    def load(cls) -> "Config":
        _take_over_legacy_config()
        try:
            # utf-8-sig: 手で編集したときの BOM 付きファイルも読めるように
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            return cls()
        known = {f for f in cls.__dataclass_fields__}
        return cls(**{k: v for k, v in data.items() if k in known})

    def save(self) -> None:
        """保存に失敗しても、その回の動作は続けられるようにする。

        書き込み不可の場所 (Program Files 直下など) に置かれた場合は黙って
        諦める。その回の変更は効いたままで、次回起動時に既定値へ戻るだけ。
        """
        try:
            CONFIG_PATH.write_text(
                json.dumps(asdict(self), indent=2), encoding="utf-8"
            )
        except OSError:
            pass
