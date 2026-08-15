"""転んだときだけ書き残す、小さなログ。

常駐アプリなので標準エラー出力は誰も見ていない (配布用の exe はコンソールを
持たない)。今回の「表示が固まったまま戻らない」ように、後から原因を追えないと
困る不具合があるため、想定外の例外だけをファイルに残す。

置き場所は config.json と同じ (exe と同じフォルダー)。やめるときはフォルダーごと
消せば痕跡が残らない、という方針を崩さないため。放っておくと際限なく育つので
上限を決めて、超えたら捨てて書き直す。
"""

from __future__ import annotations

import datetime as dt
import threading
import traceback

from config import CONFIG_PATH

LOG_PATH = CONFIG_PATH.with_name("MuuTask.log")
MAX_BYTES = 256 * 1024

_lock = threading.Lock()


def write(message: str) -> None:
    """1 件書き残す。書けない場所に置かれていたら黙って諦める。"""
    stamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    try:
        with _lock:
            if LOG_PATH.exists() and LOG_PATH.stat().st_size > MAX_BYTES:
                LOG_PATH.unlink()
            with LOG_PATH.open("a", encoding="utf-8") as out:
                out.write(f"[{stamp}] {message}\n")
    except OSError:
        pass


def exception(message: str) -> None:
    """いま処理中の例外を、スタック トレースごと書き残す。"""
    write(f"{message}\n{traceback.format_exc().rstrip()}")
