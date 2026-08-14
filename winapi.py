"""タスクバーに重ねて表示するための Win32 まわりの薄いラッパー。"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from typing import Optional

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

HWND_TOP = 0
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000

MONITOR_DEFAULTTONEAREST = 2
GA_ROOT = 2

# 既定のままだと HWND_TOPMOST (-1) が 64bit ポインタに正しく伸長されない
user32.SetWindowPos.argtypes = [
    wintypes.HWND,
    wintypes.HWND,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_uint,
]
user32.SetWindowPos.restype = wintypes.BOOL
user32.FindWindowW.restype = wintypes.HWND
user32.FindWindowExW.restype = wintypes.HWND
user32.GetForegroundWindow.restype = wintypes.HWND
user32.MonitorFromWindow.restype = wintypes.HANDLE


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


user32.WindowFromPoint.argtypes = [POINT]
user32.WindowFromPoint.restype = wintypes.HWND
user32.GetAncestor.restype = wintypes.HWND


class MONITORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("rcMonitor", wintypes.RECT),
        ("rcWork", wintypes.RECT),
        ("dwFlags", wintypes.DWORD),
    ]


Rect = tuple[int, int, int, int]


def _rect_of(hwnd) -> Optional[Rect]:
    if not hwnd:
        return None
    rect = wintypes.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return None
    return (rect.left, rect.top, rect.right, rect.bottom)


def taskbar() -> Optional[wintypes.HWND]:
    return user32.FindWindowW("Shell_TrayWnd", None) or None


def taskbar_rect() -> Optional[Rect]:
    """タスクバー本体の矩形。自動的に隠れている場合は None。"""
    hwnd = taskbar()
    if not hwnd or not user32.IsWindowVisible(hwnd):
        return None
    rect = _rect_of(hwnd)
    if rect is None:
        return None
    left, top, right, bottom = rect
    if right - left <= 0 or bottom - top <= 0:
        return None
    return rect


def child_rect(class_name: str) -> Optional[Rect]:
    """タスクバーの子ウィンドウ (通知領域など) の矩形。"""
    hwnd = taskbar()
    if not hwnd:
        return None
    return _rect_of(user32.FindWindowExW(hwnd, None, class_name, None))


def make_tool_window(hwnd: int) -> None:
    """Alt+Tab に出さず、クリックしてもフォーカスを奪わないようにする。"""
    style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    user32.SetWindowLongW(
        hwnd, GWL_EXSTYLE, style | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
    )


def toplevel_hwnd(hwnd: int) -> int:
    """Tk の winfo_id() は子ウィンドウを返すことがあるので、本体を取り直す。

    子ウィンドウのハンドルに対して SetWindowPos を呼んでも重なり順は変わらない。
    """
    root = user32.GetAncestor(hwnd, GA_ROOT)
    return int(root) if root else int(hwnd)


def is_covered(hwnd: int, x: int, y: int) -> bool:
    """指定座標で、そのウィンドウが他のウィンドウに覆われているか。"""
    top = user32.WindowFromPoint(POINT(int(x), int(y)))
    if not top:
        return True
    root = user32.GetAncestor(top, GA_ROOT)
    return int(root or top) != int(hwnd)


def raise_to_top(hwnd: int) -> None:
    """最前面グループの先頭へ入れ直す。

    タスクバーをクリックするとタスクバー自身が持ち上がり、こちらが下に潜る。
    既に最前面のウィンドウに HWND_TOPMOST を指定しても並び替えは起きないので、
    HWND_TOP を使う。
    """
    flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE
    if not user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST:
        user32.SetWindowPos(hwnd, wintypes.HWND(HWND_TOPMOST), 0, 0, 0, 0, flags)
    user32.SetWindowPos(hwnd, wintypes.HWND(HWND_TOP), 0, 0, 0, 0, flags)


def foreground_is_fullscreen() -> bool:
    """全画面のアプリ (ゲームや動画) が前面にあるか。"""
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return False
    cls = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, cls, 256)
    if cls.value in ("Shell_TrayWnd", "WorkerW", "Progman", "TkTopLevel"):
        return False
    rect = _rect_of(hwnd)
    if rect is None:
        return False
    monitor = user32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
    info = MONITORINFO()
    info.cbSize = ctypes.sizeof(MONITORINFO)
    if not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
        return False
    m = info.rcMonitor
    return (
        rect[0] <= m.left
        and rect[1] <= m.top
        and rect[2] >= m.right
        and rect[3] >= m.bottom
    )


def sample_color(points: list[tuple[int, int]]) -> Optional[str]:
    """画面上の複数点の色を調べ、いちばん多い色を #rrggbb で返す。"""
    dc = user32.GetDC(None)
    if not dc:
        return None
    counts: dict[str, int] = {}
    try:
        for x, y in points:
            value = gdi32.GetPixel(dc, int(x), int(y))
            if value == 0xFFFFFFFF:  # CLR_INVALID
                continue
            color = "#%02x%02x%02x" % (
                value & 0xFF,
                (value >> 8) & 0xFF,
                (value >> 16) & 0xFF,
            )
            counts[color] = counts.get(color, 0) + 1
    finally:
        user32.ReleaseDC(None, dc)
    if not counts:
        return None
    return max(counts.items(), key=lambda item: item[1])[0]
