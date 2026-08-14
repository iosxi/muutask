"""exe とショートカット用の muutask.ico を作る。

アプリ内で使っている音符アイコン (icons.note_icon) をそのまま流用するので、
トレイのアイコンと見た目が揃う。

    .venv\\Scripts\\python.exe make_icon.py
"""

from __future__ import annotations

from pathlib import Path

import icons

ICON_PATH = Path(__file__).with_name("muutask.ico")
BASE_SIZE = 256
SIZES = [16, 24, 32, 48, 64, 128, 256]
BG = "#1f6feb"  # 下地
FG = "#ffffff"  # 音符


def main() -> None:
    image = icons.note_icon(BASE_SIZE, FG, BG, radius=int(BASE_SIZE * 0.22))
    image.save(ICON_PATH, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"{ICON_PATH} を書き出しました")


if __name__ == "__main__":
    main()
