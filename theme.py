"""Windows のテーマ設定 (ダーク/ライト・アクセント カラー) と DPI まわりの小道具。"""

from __future__ import annotations

import ctypes
import winreg
from ctypes import wintypes
from dataclasses import dataclass

PERSONALIZE_KEY = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"
DWM_KEY = r"Software\Microsoft\Windows\DWM"

DEFAULT_ACCENT = "#4cc2ff"


def _read_dword(root, path: str, name: str):
    try:
        with winreg.OpenKey(root, path) as key:
            value, _ = winreg.QueryValueEx(key, name)
            return int(value)
    except OSError:
        return None


def is_dark_mode() -> bool:
    value = _read_dword(winreg.HKEY_CURRENT_USER, PERSONALIZE_KEY, "AppsUseLightTheme")
    return value == 0 if value is not None else True


def accent_color() -> str:
    """Windows のアクセント カラーを #rrggbb で返す。"""
    value = _read_dword(winreg.HKEY_CURRENT_USER, DWM_KEY, "AccentColor")
    if value is None:
        return DEFAULT_ACCENT
    # AccentColor は 0xAABBGGRR
    r, g, b = value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF
    return f"#{r:02x}{g:02x}{b:02x}"


def mix(color_a: str, color_b: str, ratio: float) -> str:
    """2 色を ratio (0..1) で混ぜる。ratio=1 で color_b。"""
    a = tuple(int(color_a[i : i + 2], 16) for i in (1, 3, 5))
    b = tuple(int(color_b[i : i + 2], 16) for i in (1, 3, 5))
    c = tuple(round(x + (y - x) * ratio) for x, y in zip(a, b))
    return "#%02x%02x%02x" % c


@dataclass(frozen=True)
class Palette:
    dark: bool
    accent: str
    accent_hover: str
    card: str
    border: str
    title: str
    body: str
    muted: str
    track: str
    hover: str
    disabled: str

    @classmethod
    def current(cls) -> "Palette":
        dark = is_dark_mode()
        accent = accent_color()
        if dark:
            # 暗いアクセント カラーだと進捗バーが沈むので少し明るくする
            accent = mix(accent, "#ffffff", 0.15)
            return cls(
                dark=True,
                accent=accent,
                accent_hover=mix(accent, "#ffffff", 0.25),
                card="#1f1f22",
                border="#3a3a3e",
                title="#f5f5f7",
                body="#c3c3c8",
                muted="#8b8b92",
                track="#3a3a40",
                hover="#ffffff",
                disabled="#5a5a60",
            )
        accent = mix(accent, "#000000", 0.1)
        return cls(
            dark=False,
            accent=accent,
            accent_hover=mix(accent, "#000000", 0.2),
            card="#fbfbfd",
            border="#d6d6da",
            title="#16161a",
            body="#4a4a52",
            muted="#76767e",
            track="#dcdce2",
            hover="#000000",
            disabled="#b4b4bc",
        )


# --------------------------------------------------------------------- DPI / 画面


def enable_dpi_awareness() -> None:
    """高 DPI でぼやけないようにする。ウィンドウ作成前に呼ぶこと。"""
    try:
        # PER_MONITOR_AWARE_V2
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return
    except (AttributeError, OSError):
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def ui_scale() -> float:
    try:
        return max(1.0, ctypes.windll.user32.GetDpiForSystem() / 96.0)
    except (AttributeError, OSError):
        return 1.0


def work_area() -> tuple[int, int, int, int]:
    """タスクバーを除いた画面領域 (left, top, right, bottom)。"""
    rect = wintypes.RECT()
    try:
        ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0)
    except (AttributeError, OSError):
        return (0, 0, 1920, 1040)
    if rect.right <= rect.left or rect.bottom <= rect.top:
        return (0, 0, 1920, 1040)
    return (rect.left, rect.top, rect.right, rect.bottom)


def round_window_corners(hwnd: int, width: int, height: int, radius: int) -> None:
    """枠なしウィンドウの角を丸くする (SetWindowRgn)。"""
    try:
        region = ctypes.windll.gdi32.CreateRoundRectRgn(
            0, 0, width + 1, height + 1, radius * 2, radius * 2
        )
        ctypes.windll.user32.SetWindowRgn(hwnd, region, True)
    except (AttributeError, OSError):
        pass
