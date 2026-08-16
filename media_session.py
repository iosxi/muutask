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
import io
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

import winrt.runtime
from PIL import Image
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
)
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
from winrt.windows.storage.streams import Buffer, InputStreamOptions

import errlog

# 100 ナノ秒刻み (WinRT の TimeSpan) → 秒
TICKS_PER_SECOND = 10_000_000

#: 曲が変わってからアルバム アートを読み直し続ける時間 (秒)
ART_RETRY_WINDOW = 8.0
#: その間の読み直し間隔 (秒)。遅れて届くアートを早く拾うため短くする
ART_POLL = 0.3
#: 絵柄を見比べるときの一辺 (px)。大きさの違いを均すために縮めてから比べる
ART_PRINT_SIZE = 16
#: 「同じ絵」と見なす、画素値の平均差の上限 (0-255)。取り違えると前の曲の
#: アートを出したままになるので、ほぼ同一のときだけ通す狭さにしておく
ART_SAME_LIMIT = 6.0

#: WinRT の非同期呼び出しを待つ上限 (秒)。相手のアプリが応答しなくなっても
#: ここで見切りをつけて、監視を続けられるようにする
WINRT_TIMEOUT = 5.0
#: 1 周ぶんの上限 (秒)。1 周で WinRT を数回呼ぶので、その合計より長くとる。
#: 個々の呼び出しの網から漏れたときの、最後の受け皿
CYCLE_TIMEOUT = 20.0
#: 続けてこの回数しくじったら、セッションを張り直す
RESET_AFTER = 3
#: 続けてこの回数、再生情報を読めなかったらセッションを張り直す
UNREADABLE_LIMIT = 5
#: ワーカーごと落ちたときに、立て直すまで置く間隔 (秒)
RESTART_DELAY = 1.0

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
    # ブラウザーのアプリ (PWA / 拡張) は "Chrome._crx_xxxx" の形で来る。
    # 区切りの "." が末尾に残ると、この後の split で空になってしまう
    name = name.rstrip(".")
    return name.split(".")[-1] or app_id


def _art_pixels(data: Optional[bytes]) -> int:
    """アルバム アートの画素数。読めなければ 0。

    ヘッダだけ見れば大きさは分かるので、画像本体は展開しない。
    """
    if not data:
        return 0
    try:
        with Image.open(io.BytesIO(data)) as image:
            return image.size[0] * image.size[1]
    except (OSError, ValueError):
        return 0


def _art_print(data: Optional[bytes]) -> Optional[bytes]:
    """絵柄の指紋。大きさが違っても、同じ絵なら近い値になる。"""
    if not data:
        return None
    try:
        with Image.open(io.BytesIO(data)) as image:
            small = image.convert("RGB").resize(
                (ART_PRINT_SIZE, ART_PRINT_SIZE), Image.LANCZOS
            )
        return small.tobytes()
    except (OSError, ValueError):
        return None


def _same_picture(a: Optional[bytes], b: Optional[bytes]) -> bool:
    """2 つの指紋が同じ絵か。片方でも読めなければ「違う」とする。"""
    if a is None or b is None or len(a) != len(b):
        return False
    diff = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    return diff <= ART_SAME_LIMIT


class _Stalled(Exception):
    """WinRT の非同期呼び出しが返ってこなかった。"""


def _discard(task) -> None:
    """見捨てた待ち受けの結果を捨てる (未処理例外の警告を出さないため)。"""
    if not task.cancelled():
        task.exception()


async def _call(op, timeout: float = WINRT_TIMEOUT):
    """WinRT の非同期呼び出しを、必ず返ってくるようにする。

    asyncio.wait_for は使えない。pywinrt のラッパーは取り消されると
    IAsyncOperation.cancel() を呼んでから **完了を待ち直す** ので、相手が
    応答しないままだと取り消しごと道連れで固まる。ここでは待つのをやめる
    だけにして、残った待ち受けは放っておく。
    """
    task = asyncio.ensure_future(op)
    task.add_done_callback(_discard)
    done, _pending = await asyncio.wait((task,), timeout=timeout)
    if not done:
        raise _Stalled(f"{timeout} 秒待っても返ってきませんでした")
    return task.result()


def _app_id(session) -> str:
    """セッションのアプリ ID。閉じかけのセッションでも落ちないようにする。"""
    try:
        return session.source_app_user_model_id or ""
    except OSError:
        return ""


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
        # (解除する関数, トークン) の組
        self._manager_tokens: list = []
        self._session_tokens: list = []
        # WinRT のイベント ハンドラは参照を保持しておかないと GC される
        self._manager_handlers: list = []
        self._session_handlers: list = []

        self._preferred_app: Optional[str] = None  # ユーザーが固定したセッション
        self._track_key = ""
        # 直近の確かなタイムライン (track_key, 長さ, 位置, time.monotonic())
        self._timeline: Optional[tuple[str, float, float, float]] = None
        self._thumbnail: Optional[bytes] = None
        self._art_deadline = 0.0  # この時刻まではアルバム アートを読み直す
        self._art_pixels = 0  # いま持っているアートの画素数
        self._art_print: Optional[bytes] = None  # その絵柄の指紋
        self._failures = 0  # 続けてしくじった回数
        self._unreadable = 0  # 続けて再生情報を読めなかった回数
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
        await self._safe_update()

    async def _cmd(self, name: str, arg: float = 0.0) -> None:
        session = self._session
        if session is not None:
            try:
                if name == "toggle":
                    await _call(session.try_toggle_play_pause_async())
                elif name == "next":
                    await _call(session.try_skip_next_async())
                elif name == "prev":
                    await _call(session.try_skip_previous_async())
                elif name == "seek":
                    await _call(
                        session.try_change_playback_position_async(
                            int(max(0.0, arg) * TICKS_PER_SECOND)
                        )
                    )
            except Exception:
                # アプリ側が要求を受け付けない場合がある。次の更新で追いつく。
                errlog.exception(f"操作 {name} を送れませんでした")
        # 操作直後は状態が変わるので少し待ってから読み直す
        await asyncio.sleep(0.15)
        await self._safe_update()

    async def _safe_update(self) -> None:
        """例外を持ち出さない更新。ワーカーの周回の外から呼ぶとき用。"""
        try:
            await self._update()
        except Exception:
            self._note_failure()

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
        # ワーカーが何かの拍子に落ちても、ここで立て直す。監視が止まると
        # 表示は最後の曲のまま、ボタンも効かないまま二度と戻らないので、
        # 諦めてよいのは終了するときだけ。
        while not self._stopping:
            loop = asyncio.new_event_loop()
            self._loop = loop
            asyncio.set_event_loop(loop)
            try:
                loop.run_until_complete(self._worker())
            except Exception:
                errlog.exception("メディア監視が落ちました。立て直します")
            finally:
                self._loop = None
                self._drop_manager()
                try:
                    loop.close()
                except Exception:
                    pass
            if not self._stopping:
                time.sleep(RESTART_DELAY)

    async def _cycle(self) -> None:
        """監視の 1 周。"""
        if self._manager is None:
            await self._open_manager()
        await self._update()

    async def _worker(self) -> None:
        self._dirty = asyncio.Event()
        cycle = None
        while not self._stopping:
            if cycle is None or cycle.done():
                cycle = asyncio.ensure_future(self._cycle())
                cycle.add_done_callback(_discard)
            # 1 周が返ってこないことも起こりうる。個々の呼び出しにも上限は
            # 設けてあるが、どこか一箇所でも待ちっぱなしになると監視が止まり、
            # 表示も操作も凍りつく。周回そのものにも網をかけ、返ってこない
            # 周回は見捨てて先へ進む。
            done, _pending = await asyncio.wait((cycle,), timeout=CYCLE_TIMEOUT)
            if not done:
                self._note_failure("1 周が返ってきません", trace=False)
                if self._manager is None:  # 張り直しに入った
                    cycle = None  # 詰まった周回は見捨てる
            else:
                try:
                    cycle.result()
                    self._failures = 0
                except Exception:
                    self._note_failure("監視でしくじりました")
            self._dirty.clear()
            # アートの読み直し中だけ間隔を詰める (遅れて届く分を早く拾う)
            wait = ART_POLL if time.monotonic() < self._art_deadline else self.TICK
            try:
                await asyncio.wait_for(self._dirty.wait(), timeout=wait)
            except asyncio.TimeoutError:
                pass

    def _skip_unreadable(self) -> None:
        """再生情報を読めなかった周回を、静かに飛ばす。

        閉じかけのセッションでは `get_playback_info()` が None を返してくる
        (実測。v13 のログには 1 日で 5 回残っていた)。例外にして周回ごと
        転ばせると、3 回続いたときにセッションを張り直すことになり、そのとき
        アルバム アートも覚えている再生位置も捨ててしまう。捨てた直後に
        Firefox の空タイムラインが来ると、せっかく直したバーがまた頭に戻る。

        読めないだけなら、その周回を飛ばして次に賭ければよい。表示は 1 秒
        ぶん古いままになるだけで、誰も困らない。ただし死んだセッションを
        掴んだまま二度と読めない目もあるので、続くようなら張り直す。
        """
        self._unreadable += 1
        if self._unreadable >= UNREADABLE_LIMIT:
            self._unreadable = 0
            errlog.write("再生情報を読めません。セッションを張り直します")
            self._drop_manager()

    def _note_failure(
        self, reason: str = "監視でしくじりました", trace: bool = True
    ) -> None:
        """周回でしくじったときの後始末。

        ここで取りこぼすとワーカー スレッドごと死ぬ。そうなると表示は止まった
        まま、ボタンも効かないまま二度と戻らない (実際にそうなった)。相手の
        アプリは曲を切り替えるたびにセッションを作り直すので、閉じかけの
        セッションを触って想定外の例外が飛んでくることがある。何が起きても
        監視は続け、続けて転ぶようならセッションごと張り直す。
        """
        self._failures += 1
        message = f"{reason} ({self._failures} 回目)"
        if trace:
            errlog.exception(message)
        else:
            errlog.write(message)
        if self._failures >= RESET_AFTER:
            self._failures = 0
            errlog.write("セッションを張り直します")
            self._drop_manager()

    # -------------------------------------------------------------- 張り直し

    async def _open_manager(self) -> None:
        self._manager = await _call(MediaManager.request_async())
        for adder, remover in (
            (self._manager.add_sessions_changed, self._manager.remove_sessions_changed),
            (
                self._manager.add_current_session_changed,
                self._manager.remove_current_session_changed,
            ),
        ):
            handler = self._wake_threadsafe
            self._manager_handlers.append(handler)
            try:
                self._manager_tokens.append((remover, adder(handler)))
            except OSError:
                pass

    def _drop_manager(self) -> None:
        """監視をいったん手放す。次の周回で取り直される。"""
        self._detach_session()
        tokens, self._manager_tokens = self._manager_tokens, []
        self._manager_handlers.clear()
        for remover, token in tokens:
            try:
                remover(token)
            except Exception:
                pass
        self._manager = None
        self._track_key = ""
        self._timeline = None
        self._unreadable = 0
        self._forget_art()

    # -------------------------------------------------------------- セッション選択

    def _pick_session(self):
        """監視対象のセッションと、選択できるセッション一覧を返す。"""
        sessions = [s for s in self._manager.get_sessions() if _app_id(s)]
        listing = tuple((_app_id(s), friendly_app_name(_app_id(s))) for s in sessions)

        if self._preferred_app:
            for s in sessions:
                if _app_id(s) == self._preferred_app:
                    return s, listing

        def is_playing(s) -> bool:
            try:
                info = s.get_playback_info()
            except OSError:
                return False
            # 閉じかけのセッションは None を返してくる (実測)
            return info is not None and info.playback_status == PlaybackStatus.PLAYING

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
        if session is None:
            return
        for adder, remover in (
            (
                session.add_media_properties_changed,
                session.remove_media_properties_changed,
            ),
            (session.add_playback_info_changed, session.remove_playback_info_changed),
            (
                session.add_timeline_properties_changed,
                session.remove_timeline_properties_changed,
            ),
        ):
            handler = self._wake_threadsafe
            self._session_handlers.append(handler)
            try:
                self._session_tokens.append((remover, adder(handler)))
            except OSError:
                pass

    def _detach_session(self) -> None:
        self._session = None
        tokens, self._session_tokens = self._session_tokens, []
        self._session_handlers.clear()
        for remover, token in tokens:
            try:
                remover(token)
            except Exception:
                pass

    # ------------------------------------------------------------------ 状態取得

    async def _update(self) -> None:
        if self._manager is None:
            return

        session, listing = self._pick_session()
        # アプリ ID ではなくセッションそのものを見比べる。同じアプリでも曲を
        # 変えるとセッションは作り直されるので、ID で見ていると閉じた方を
        # 掴んだままになり、イベントも届かず、ボタンの操作も宛先を失う。
        if session is None or self._session is None or session != self._session:
            self._attach_session(session)

        if session is None:
            self._track_key = ""
            self._timeline = None
            self._forget_art()
            self._publish(NowPlaying(sessions=listing))
            return

        info = session.get_playback_info()
        if info is None:
            self._skip_unreadable()
            return
        self._unreadable = 0
        status = {
            PlaybackStatus.PLAYING: "playing",
            PlaybackStatus.PAUSED: "paused",
        }.get(info.playback_status, "stopped")
        controls = info.controls

        duration = position = 0.0
        timeline = session.get_timeline_properties()
        # ここも None が返ることがある。長さも位置も分からないだけなので 0 の
        # まま先へ進め、_steady_timeline に直前の値を継がせる
        if timeline is not None:
            start = timeline.start_time.total_seconds()
            duration = max(0.0, timeline.end_time.total_seconds() - start)
            position = max(0.0, timeline.position.total_seconds() - start)
            if status == "playing":
                position += self._elapsed_since(timeline.last_updated_time)
            if duration > 0:
                position = min(position, duration)

        props = await _call(session.try_get_media_properties_async())
        title = (props.title or "") if props is not None else ""
        artist = (props.artist or props.album_artist or "") if props is not None else ""
        album = (props.album_title or "") if props is not None else ""
        app_id = _app_id(session)
        track_key = " | ".join((app_id, title, artist, album))
        if track_key != self._track_key:
            # 曲が変わった。アートはすぐには追いついてこないので、しばらく
            # 読み直す。実測では 0.1〜0.5 秒ほど遅れて本来の画像が届く
            self._track_key = track_key
            self._art_deadline = time.monotonic() + ART_RETRY_WINDOW
            await self._refresh_art(props, fresh=True)
        elif time.monotonic() < self._art_deadline:
            await self._refresh_art(props, fresh=False)

        duration, position = self._steady_timeline(
            track_key, status, duration, position
        )

        self._publish(
            NowPlaying(
                app_id=app_id,
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

    def _steady_timeline(
        self, track_key: str, status: str, duration: float, position: float
    ) -> tuple[float, float]:
        """空っぽのタイムラインで、いままでの再生位置を消さないようにする。

        Firefox は Google からログアウトした状態の YouTube / YouTube Music で
        シークすると、再生を続けたまま「長さも位置も 0」のタイムラインを
        送ってくる (実測。しかも last_updated_time だけは今の時刻で更新される
        ので、古い報せとして見分けることもできない)。素直に受け取ると曲の
        途中なのにバーが頭に戻り、一時停止して再生し直すまで戻らない。

        長さの分かっている曲でそれが来たら、届かなかったものと見なして直前の
        値を進め続ける。空で塗り潰される 0.25 秒前に正しい位置が 1 度だけ届く
        (タイムラインの変更通知で起こされるので、たいていは拾える) ので、
        ふつうはシークした先から数え直せる。その 1 度を取りこぼしても、頭に
        戻って固まることはなく、次のまともな報せ (一時停止・再生や曲の
        変わり目) で正しい位置に戻る。

        長さの分からない曲 (ライブ配信など) は元から 0 なので、そのまま通す。
        """
        now = time.monotonic()
        if duration > 0:
            self._timeline = (track_key, duration, position, now)
            return duration, position

        held = self._timeline
        if held is None or held[0] != track_key:
            self._timeline = None
            return duration, position

        _key, held_duration, held_position, held_at = held
        if status == "playing":
            held_position += max(0.0, now - held_at)
        held_position = min(held_position, held_duration)
        self._timeline = (track_key, held_duration, held_position, now)
        return held_duration, held_position

    async def _refresh_art(self, props, fresh: bool) -> None:
        """アルバム アートを読み直し、良くなるときだけ差し替える。

        同じ絵が 256x256 → 150x150 → 120x120 と、だんだん粗くなりながら
        届くアプリがある。届いた順に採ると、小窓のアートが一瞬だけキレイで
        すぐぼやけ、読み直しの時間が過ぎるとその粗いままで固定されてしまう。
        そこで、いま持っているものと同じ絵で、かつ小さいものは見送る。
        絵柄そのものが変わったときは、小さくなっていても素直に差し替える
        (見送ると前の曲のアートを出し続けることになってしまう)。
        """
        latest = await self._read_thumbnail(props)
        if latest is None:
            if fresh:  # アートの無い曲に変わった
                self._thumbnail = None
                self._art_pixels = 0
                self._art_print = None
            return
        if latest == self._thumbnail:
            return

        pixels = _art_pixels(latest)
        fingerprint = _art_print(latest)
        if pixels < self._art_pixels and _same_picture(fingerprint, self._art_print):
            return
        self._thumbnail = latest
        self._art_pixels = pixels
        self._art_print = fingerprint

    def _forget_art(self) -> None:
        """アルバム アートの持ち物を捨てる。次の曲でまっさらから拾い直す。"""
        self._thumbnail = None
        self._art_deadline = 0.0
        self._art_pixels = 0
        self._art_print = None

    @staticmethod
    def _elapsed_since(updated) -> float:
        """タイムラインが最後に更新されてからの秒数。当てにならなければ 0。

        last_updated_time の中身は相手のアプリ任せで、タイム ゾーン情報の無い
        値や桁の壊れた値が来ることがある。素で引き算すると TypeError や
        OverflowError になり、監視ごと道連れになるのでここで受け止める。
        """
        if updated is None or updated.tzinfo is None:
            return 0.0
        try:
            elapsed = (dt.datetime.now(dt.timezone.utc) - updated).total_seconds()
        except (TypeError, ValueError, OverflowError, OSError):
            return 0.0
        return elapsed if 0.0 <= elapsed < 3600.0 else 0.0

    async def _read_thumbnail(self, props) -> Optional[bytes]:
        if props is None:
            return None
        ref = props.thumbnail
        if ref is None:
            return None
        try:
            stream = await _call(ref.open_read_async())
            size = int(stream.size)
            if size <= 0 or size > 32 * 1024 * 1024:
                return None
            buffer = Buffer(size)
            await _call(
                stream.read_async(buffer, size, InputStreamOptions.READ_AHEAD)
            )
            return bytes(memoryview(buffer))
        except OSError:
            return None

    def _publish(self, state: NowPlaying) -> None:
        self._state = state
        try:
            self._on_update(state)
        except Exception:  # UI 側の例外でワーカーを落とさない
            pass
