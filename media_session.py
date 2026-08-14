"""Windows のグローバル メディア セッション (GSMTC) を監視・操作するワーカー。

Spotify / ブラウザ / メディア プレーヤーなど、Windows のメディア コントロール
(音量ポップアップに出るあれ) に対応しているアプリの再生情報をまとめて扱う。

WinRT の呼び出しは専用スレッド上の asyncio ループで行い、更新は
`on_update` コールバックでスナップショット (NowPlaying) として通知する。
コールバックはワーカー スレッドから呼ばれるので、UI 側でマーシャリングすること。
"""

from __future__ import annotations

import asyncio
import datetime as dt
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

import winrt.runtime
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
)
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
from winrt.windows.storage.streams import Buffer, InputStreamOptions

# 100 ナノ秒刻み (WinRT の TimeSpan) → 秒
TICKS_PER_SECOND = 10_000_000

#: 曲が変わってからアルバム アートを読み直し続ける時間 (秒)
ART_RETRY_WINDOW = 8.0
#: その間の読み直し間隔 (秒)。遅れて届くアートを早く拾うため短くする
ART_POLL = 0.3

# アプリ ID からそれっぽい表示名を作るための対応表
FRIENDLY_NAMES = {
    "spotify.exe": "Spotify",
    "chrome.exe": "Chrome",
    "msedge.exe": "Edge",
    "firefox.exe": "Firefox",
    "vlc.exe": "VLC",
    "foobar2000.exe": "foobar2000",
    "wmplayer.exe": "Windows Media Player",
    "itunes.exe": "iTunes",
    "aimp.exe": "AIMP",
    "mpc-hc64.exe": "MPC-HC",
    "microsoft.zunemusic": "メディア プレーヤー",
    "microsoft.zunemusic_8wekyb3d8bbwe!microsoft.zunemusic": "メディア プレーヤー",
}


def friendly_app_name(app_id: str) -> str:
    """AUMID や実行ファイル名を、人が読める短い名前にする。"""
    if not app_id:
        return ""
    key = app_id.lower()
    if key in FRIENDLY_NAMES:
        return FRIENDLY_NAMES[key]
    for known, name in FRIENDLY_NAMES.items():
        if known in key:
            return name
    # "Foo.Bar_9abc!App" のような AUMID から見出しっぽい部分を拾う
    name = app_id.split("!")[-1].split("_")[0]
    if name.lower().endswith(".exe"):
        name = name[:-4]
    return name.split(".")[-1] or app_id


@dataclass(frozen=True)
class NowPlaying:
    """ある瞬間の再生状態のスナップショット。"""

    app_id: str = ""
    title: str = ""
    artist: str = ""
    album: str = ""
    status: str = "none"  # playing / paused / stopped / none
    position: float = 0.0
    duration: float = 0.0
    captured_at: float = 0.0  # time.monotonic() の値
    can_play: bool = False
    can_pause: bool = False
    can_next: bool = False
    can_prev: bool = False
    can_seek: bool = False
    thumbnail: Optional[bytes] = None
    track_key: str = ""  # 曲が変わったかどうかの判定用
    sessions: tuple[tuple[str, str], ...] = ()  # (app_id, 表示名)

    @property
    def has_media(self) -> bool:
        return self.status != "none"

    @property
    def is_playing(self) -> bool:
        return self.status == "playing"

    @property
    def art_key(self) -> str:
        """アルバム アートを描き直すかの判定用。

        曲が同じでも、遅れて届いたアートに差し替わることがあるので、
        track_key だけでなく画像の中身も見る。
        """
        thumb = self.thumbnail
        return f"{self.track_key}|{len(thumb) if thumb else 0}|{hash(thumb)}"

    @property
    def app_name(self) -> str:
        return friendly_app_name(self.app_id)

    def live_position(self) -> float:
        """スナップショット取得からの経過を足した、いまの再生位置。"""
        pos = self.position
        if self.status == "playing":
            pos += max(0.0, time.monotonic() - self.captured_at)
        if self.duration > 0:
            pos = min(pos, self.duration)
        return max(0.0, pos)


class MediaController:
    """メディア セッションを監視し、操作を送るコントローラ。"""

    #: セッションの取りこぼしに備えた保険のポーリング間隔 (秒)
    TICK = 1.0

    def __init__(self, on_update: Callable[[NowPlaying], None]) -> None:
        self._on_update = on_update
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread = threading.Thread(
            target=self._thread_main, name="muutask-media", daemon=True
        )
        self._stopping = False

        self._manager = None
        self._session = None
        self._session_id = ""
        self._session_tokens: list = []
        # WinRT のイベント ハンドラは参照を保持しておかないと GC される
        self._manager_handlers: list = []
        self._session_handlers: list = []

        self._preferred_app: Optional[str] = None  # ユーザーが固定したセッション
        self._track_key = ""
        self._thumbnail: Optional[bytes] = None
        self._art_deadline = 0.0  # この時刻まではアルバム アートを読み直す
        self._dirty: Optional[asyncio.Event] = None
        self._state = NowPlaying()

    # ------------------------------------------------------------------ 開始/終了

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stopping = True
        loop = self._loop
        if loop is not None and loop.is_running():
            loop.call_soon_threadsafe(self._wake)

    # ------------------------------------------------------------------ 操作 API

    def toggle_play_pause(self) -> None:
        self._submit(self._cmd("toggle"))

    def next_track(self) -> None:
        self._submit(self._cmd("next"))

    def previous_track(self) -> None:
        self._submit(self._cmd("prev"))

    def seek(self, seconds: float) -> None:
        self._submit(self._cmd("seek", seconds))

    def select_session(self, app_id: Optional[str]) -> None:
        """特定アプリのセッションに固定する。None で自動選択に戻す。"""
        self._preferred_app = app_id
        self._submit(self._refresh_now())

    @property
    def state(self) -> NowPlaying:
        return self._state

    # ------------------------------------------------------------------ 内部

    def _submit(self, coro) -> None:
        loop = self._loop
        if loop is None or not loop.is_running():
            coro.close()
            return
        try:
            asyncio.run_coroutine_threadsafe(coro, loop)
        except RuntimeError:
            coro.close()

    async def _refresh_now(self) -> None:
        await self._update()

    async def _cmd(self, name: str, arg: float = 0.0) -> None:
        session = self._session
        if session is None:
            return
        try:
            if name == "toggle":
                await session.try_toggle_play_pause_async()
            elif name == "next":
                await session.try_skip_next_async()
            elif name == "prev":
                await session.try_skip_previous_async()
            elif name == "seek":
                await session.try_change_playback_position_async(
                    int(max(0.0, arg) * TICKS_PER_SECOND)
                )
        except OSError:
            # アプリ側が要求を受け付けない場合がある。次の更新で状態は追いつく。
            pass
        # 操作直後は状態が変わるので少し待ってから読み直す
        await asyncio.sleep(0.15)
        await self._update()

    def _wake(self) -> None:
        if self._dirty is not None:
            self._dirty.set()

    def _wake_threadsafe(self, *_args) -> None:
        """WinRT のイベント スレッドから呼ばれる。"""
        loop = self._loop
        if loop is not None and loop.is_running():
            try:
                loop.call_soon_threadsafe(self._wake)
            except RuntimeError:
                pass

    def _thread_main(self) -> None:
        try:
            winrt.runtime.init_apartment(winrt.runtime.ApartmentType.MULTI_THREADED)
        except OSError:
            pass
        loop = asyncio.new_event_loop()
        self._loop = loop
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._worker())
        finally:
            self._detach_session()
            loop.close()
            self._loop = None

    async def _worker(self) -> None:
        self._dirty = asyncio.Event()
        try:
            self._manager = await MediaManager.request_async()
        except OSError:
            self._publish(NowPlaying())
            return

        for adder in (
            self._manager.add_sessions_changed,
            self._manager.add_current_session_changed,
        ):
            handler = self._wake_threadsafe
            self._manager_handlers.append(handler)
            adder(handler)

        while not self._stopping:
            try:
                await self._update()
            except OSError:
                pass
            self._dirty.clear()
            # アートの読み直し中だけ間隔を詰める (遅れて届く分を早く拾う)
            wait = ART_POLL if time.monotonic() < self._art_deadline else self.TICK
            try:
                await asyncio.wait_for(self._dirty.wait(), timeout=wait)
            except asyncio.TimeoutError:
                pass

    # -------------------------------------------------------------- セッション選択

    def _pick_session(self):
        """監視対象のセッションと、選択できるセッション一覧を返す。"""
        sessions = list(self._manager.get_sessions())
        listing = tuple(
            (s.source_app_user_model_id, friendly_app_name(s.source_app_user_model_id))
            for s in sessions
        )

        if self._preferred_app:
            for s in sessions:
                if s.source_app_user_model_id == self._preferred_app:
                    return s, listing

        def is_playing(s) -> bool:
            try:
                return s.get_playback_info().playback_status == PlaybackStatus.PLAYING
            except OSError:
                return False

        current = self._manager.get_current_session()
        if current is not None and is_playing(current):
            return current, listing
        for s in sessions:
            if is_playing(s):
                return s, listing
        if current is not None:
            return current, listing
        return (sessions[0] if sessions else None), listing

    def _attach_session(self, session) -> None:
        self._detach_session()
        self._session = session
        self._session_id = session.source_app_user_model_id if session else ""
        if session is None:
            return
        for adder in (
            session.add_media_properties_changed,
            session.add_playback_info_changed,
            session.add_timeline_properties_changed,
        ):
            handler = self._wake_threadsafe
            self._session_handlers.append(handler)
            self._session_tokens.append((adder(handler), adder.__name__))

    def _detach_session(self) -> None:
        session, self._session = self._session, None
        tokens, self._session_tokens = self._session_tokens, []
        if session is None:
            return
        removers = {
            "add_media_properties_changed": session.remove_media_properties_changed,
            "add_playback_info_changed": session.remove_playback_info_changed,
            "add_timeline_properties_changed": session.remove_timeline_properties_changed,
        }
        for token, adder_name in tokens:
            try:
                removers[adder_name](token)
            except OSError:
                pass
        self._session_handlers.clear()

    # ------------------------------------------------------------------ 状態取得

    async def _update(self) -> None:
        if self._manager is None:
            return

        session, listing = self._pick_session()
        if (
            session is None
            or self._session is None
            or session.source_app_user_model_id != self._session_id
        ):
            self._attach_session(session)

        if session is None:
            self._track_key = ""
            self._thumbnail = None
            self._art_deadline = 0.0
            self._publish(NowPlaying(sessions=listing))
            return

        info = session.get_playback_info()
        status = {
            PlaybackStatus.PLAYING: "playing",
            PlaybackStatus.PAUSED: "paused",
        }.get(info.playback_status, "stopped")
        controls = info.controls

        timeline = session.get_timeline_properties()
        start = timeline.start_time.total_seconds()
        duration = max(0.0, timeline.end_time.total_seconds() - start)
        position = max(0.0, timeline.position.total_seconds() - start)
        if status == "playing" and timeline.last_updated_time is not None:
            elapsed = (
                dt.datetime.now(dt.timezone.utc) - timeline.last_updated_time
            ).total_seconds()
            if 0 <= elapsed < 3600:
                position += elapsed
        if duration > 0:
            position = min(position, duration)

        props = await session.try_get_media_properties_async()
        title = props.title or ""
        artist = props.artist or props.album_artist or ""
        album = props.album_title or ""
        track_key = " | ".join(
            (session.source_app_user_model_id, title, artist, album)
        )
        if track_key != self._track_key:
            self._track_key = track_key
            self._thumbnail = await self._read_thumbnail(props)
            self._art_deadline = time.monotonic() + ART_RETRY_WINDOW
        elif time.monotonic() < self._art_deadline:
            # 曲名が変わった時点では、アートがまだ前の曲のまま (あるいは
            # 途中の別画像) のことがある。実測では 0.5 秒ほど遅れて本来の
            # 画像に差し替わる。1 回しか読まないと、そのズレたまま固定されて
            # しまうので、しばらく読み直して新しくなっていれば拾い直す。
            latest = await self._read_thumbnail(props)
            if latest is not None and latest != self._thumbnail:
                self._thumbnail = latest

        self._publish(
            NowPlaying(
                app_id=session.source_app_user_model_id,
                title=title,
                artist=artist,
                album=album,
                status=status,
                position=position,
                duration=duration,
                captured_at=time.monotonic(),
                can_play=bool(controls.is_play_enabled),
                can_pause=bool(controls.is_pause_enabled),
                can_next=bool(controls.is_next_enabled),
                can_prev=bool(controls.is_previous_enabled),
                can_seek=bool(controls.is_playback_position_enabled),
                thumbnail=self._thumbnail,
                track_key=track_key,
                sessions=listing,
            )
        )

    async def _read_thumbnail(self, props) -> Optional[bytes]:
        ref = props.thumbnail
        if ref is None:
            return None
        try:
            stream = await ref.open_read_async()
            size = int(stream.size)
            if size <= 0 or size > 32 * 1024 * 1024:
                return None
            buffer = Buffer(size)
            await stream.read_async(buffer, size, InputStreamOptions.READ_AHEAD)
            return bytes(memoryview(buffer))
        except OSError:
            return None

    def _publish(self, state: NowPlaying) -> None:
        self._state = state
        try:
            self._on_update(state)
        except Exception:  # UI 側の例外でワーカーを落とさない
            pass
