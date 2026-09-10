#include "media.h"

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "artwork.h"
#include "common.h"
#include "errlog.h"
#include "image.h"

using namespace std::chrono_literals;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::DateTime;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSessionMediaProperties;
using winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using winrt::Windows::Storage::Streams::Buffer;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::InputStreamOptions;

namespace media {
namespace {

// 曲が変わってからアルバム アートを読み直し続ける時間
constexpr auto kArtRetryWindow = 8000ms;
// その間の読み直し間隔。遅れて届くアートを早く拾うため短くする
constexpr auto kArtPoll = 300ms;
// セッションの取りこぼしに備えた保険のポーリング間隔
constexpr auto kTick = 1000ms;
// WinRT の非同期呼び出しを待つ上限。相手のアプリが応答しなくなっても、ここで
// 見切りをつけて監視を続けられるようにする
constexpr auto kWinrtTimeout = 5s;
// 続けてこの回数しくじったら、セッションを張り直す
constexpr int kResetAfter = 3;
// 続けてこの回数、再生情報を読めなかったらセッションを張り直す
constexpr int kUnreadableLimit = 5;
// ワーカーごと落ちたときに、立て直すまで置く間隔
constexpr auto kRestartDelay = 1000ms;
// 操作を送ったあと、状態を読み直すまで置く間隔
constexpr auto kCommandSettle = 150ms;

constexpr double kTicksPerSecond = 10'000'000.0;

/// WinRT の非同期呼び出しが返ってこなかった。
class Stalled : public std::exception {
public:
    char const* what() const noexcept override { return "WinRT が返ってきません"; }
};

/// 待つのをやめるだけで、取り消しはしない。
///
/// 取り消しを頼むと、相手が応答しない場合にその取り消し自体で固まることが
/// ある (Python 版で asyncio.wait_for が使えなかったのと同じ理由)。放って
/// おけば、いずれ相手が返したところで勝手に片付く。
template <typename Async>
auto Await(Async const& op) {
    if (op.wait_for(kWinrtTimeout) != AsyncStatus::Completed) throw Stalled();
    return op.GetResults();
}

std::wstring HStr(winrt::hstring const& value) {
    return std::wstring(value.c_str(), value.size());
}

/// セッションのアプリ ID。閉じかけのセッションでも落ちないようにする。
std::wstring AppIdOf(GlobalSystemMediaTransportControlsSession const& session) {
    if (!session) return {};
    try {
        return HStr(session.SourceAppUserModelId());
    } catch (winrt::hresult_error const&) {
        return {};
    }
}

/// 例外の説明。ログに残すため。
std::wstring Describe(std::exception_ptr const& error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (winrt::hresult_error const& e) {
        wchar_t buf[32];
        swprintf(buf, 32, L"0x%08x: ", (unsigned)e.code().value);
        return buf + HStr(e.message());
    } catch (std::exception const& e) {
        return Utf8ToWide(e.what());
    } catch (...) {
    }
    return L"(不明な失敗)";
}

// アプリ ID からそれっぽい表示名を作るための対応表
struct FriendlyName {
    wchar_t const* key;
    wchar_t const* name;
};
constexpr FriendlyName kFriendlyNames[] = {
    {L"spotify.exe", L"Spotify"},
    {L"chrome.exe", L"Chrome"},
    {L"msedge.exe", L"Edge"},
    {L"firefox.exe", L"Firefox"},
    {L"vlc.exe", L"VLC"},
    {L"foobar2000.exe", L"foobar2000"},
    {L"wmplayer.exe", L"Windows Media Player"},
    {L"itunes.exe", L"iTunes"},
    {L"aimp.exe", L"AIMP"},
    {L"mpc-hc64.exe", L"MPC-HC"},
    {L"microsoft.zunemusic", L"メディア プレーヤー"},
    {L"microsoft.zunemusic_8wekyb3d8bbwe!microsoft.zunemusic", L"メディア プレーヤー"},
};

}  // namespace

// --------------------------------------------------------------- NowPlaying

double NowPlaying::LivePosition() const {
    double pos = position;
    if (status == Status::Playing) {
        double elapsed = (double)(NowMs() - captured_at) / 1000.0;
        if (elapsed > 0) pos += elapsed;
    }
    if (duration > 0 && pos > duration) pos = duration;
    return pos < 0 ? 0 : pos;
}

std::wstring NowPlaying::AppName() const { return FriendlyAppName(app_id); }

std::wstring FriendlyAppName(std::wstring const& app_id) {
    if (app_id.empty()) return {};
    std::wstring const key = ToLower(app_id);
    for (auto const& entry : kFriendlyNames) {
        if (key == entry.key) return entry.name;
    }
    for (auto const& entry : kFriendlyNames) {
        if (key.find(entry.key) != std::wstring::npos) return entry.name;
    }

    // "Foo.Bar_9abc!App" のような AUMID から見出しっぽい部分を拾う
    std::wstring name = app_id;
    if (size_t bang = name.find_last_of(L'!'); bang != std::wstring::npos) {
        name = name.substr(bang + 1);
    }
    if (size_t under = name.find(L'_'); under != std::wstring::npos) {
        name = name.substr(0, under);
    }
    if (name.size() > 4 && ToLower(name.substr(name.size() - 4)) == L".exe") {
        name = name.substr(0, name.size() - 4);
    }
    // ブラウザーのアプリ (PWA / 拡張) は "Chrome._crx_xxxx" の形で来る。
    // 区切りの "." が末尾に残ると、この後の切り出しで空になってしまう
    while (!name.empty() && name.back() == L'.') name.pop_back();
    if (size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos) {
        name = name.substr(dot + 1);
    }
    return name.empty() ? app_id : name;
}

// --------------------------------------------------------------- Impl

struct MediaController::Impl {
    explicit Impl(UpdateHandler handler) : on_update(std::move(handler)) {
        dirty = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state = std::make_shared<NowPlaying>();
    }

    ~Impl() {
        if (dirty) CloseHandle(dirty);
    }

    // ------------------------------------------------------------ 外から

    UpdateHandler on_update;
    HANDLE dirty = nullptr;
    std::thread thread;
    std::atomic<bool> stopping{false};

    mutable std::mutex state_lock;
    std::shared_ptr<NowPlaying const> state;

    struct Command {
        enum class Kind { Toggle, Next, Prev, Seek } kind;
        double arg = 0.0;
    };
    std::mutex command_lock;
    std::vector<Command> commands;

    std::mutex preferred_lock;
    std::optional<std::wstring> preferred_app;

    // ------------------------------------------------------- ワーカーの持ち物

    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    GlobalSystemMediaTransportControlsSession session{nullptr};
    winrt::event_token manager_sessions_token{};
    winrt::event_token manager_current_token{};
    winrt::event_token session_media_token{};
    winrt::event_token session_playback_token{};
    winrt::event_token session_timeline_token{};

    std::wstring track_key;
    /// 直近の確かなタイムライン (track_key, 長さ, 位置, NowMs())
    struct Timeline {
        std::wstring key;
        double duration = 0;
        double position = 0;
        uint64_t at = 0;
    };
    std::optional<Timeline> timeline;

    std::shared_ptr<std::vector<uint8_t> const> thumbnail;
    uint64_t art_serial = 0;
    uint64_t art_deadline = 0;  // この時刻まではアルバム アートを読み直す
    int64_t art_pixels = 0;     // いま持っているアートの画素数
    std::optional<image::Rgb888> art_print;  // その絵柄の指紋

    int failures = 0;    // 続けてしくじった回数
    int unreadable = 0;  // 続けて再生情報を読めなかった回数

    // ------------------------------------------------------------ 進行

    void Wake() {
        if (dirty) SetEvent(dirty);
    }

    void Submit(Command const& command) {
        {
            std::lock_guard<std::mutex> guard(command_lock);
            commands.push_back(command);
        }
        Wake();
    }

    std::vector<Command> TakeCommands() {
        std::lock_guard<std::mutex> guard(command_lock);
        std::vector<Command> out;
        out.swap(commands);
        return out;
    }

    std::optional<std::wstring> Preferred() {
        std::lock_guard<std::mutex> guard(preferred_lock);
        return preferred_app;
    }

    void ThreadMain();
    void Worker();
    void RunCommands();
    void OpenManager();
    void DropManager();
    void AttachSession(GlobalSystemMediaTransportControlsSession const& next);
    void DetachSession();
    void Update();
    void Publish(std::shared_ptr<NowPlaying> snapshot);
    void NoteFailure(std::wstring const& reason, std::exception_ptr error);
    void SkipUnreadable();
    void ForgetArt();
    void RefreshArt(GlobalSystemMediaTransportControlsSessionMediaProperties const& props,
                    bool fresh);
    std::optional<std::vector<uint8_t>> ReadThumbnail(
        GlobalSystemMediaTransportControlsSessionMediaProperties const& props);
    std::pair<double, double> SteadyTimeline(std::wstring const& key, Status status,
                                             double duration, double position);
    std::pair<GlobalSystemMediaTransportControlsSession,
              std::vector<std::pair<std::wstring, std::wstring>>>
    PickSession();
};

// --------------------------------------------------------------- 立て直し

void MediaController::Impl::ThreadMain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // ワーカーが何かの拍子に落ちても、ここで立て直す。監視が止まると表示は
    // 最後の曲のまま、ボタンも効かないまま二度と戻らないので、諦めてよいのは
    // 終了するときだけ。
    while (!stopping) {
        try {
            Worker();
        } catch (...) {
            errlog::Exception(L"メディア監視が落ちました。立て直します",
                              Describe(std::current_exception()));
        }
        DropManager();
        if (!stopping) std::this_thread::sleep_for(kRestartDelay);
    }
    DropManager();
    winrt::uninit_apartment();
}

void MediaController::Impl::Worker() {
    while (!stopping) {
        RunCommands();
        if (stopping) break;

        try {
            if (!manager) OpenManager();
            Update();
            failures = 0;
        } catch (...) {
            NoteFailure(L"監視でしくじりました", std::current_exception());
        }

        if (stopping) break;
        // アートの読み直し中だけ間隔を詰める (遅れて届く分を早く拾う)
        auto const wait = NowMs() < art_deadline ? kArtPoll : kTick;
        WaitForSingleObject(dirty, (DWORD)wait.count());
    }
}

void MediaController::Impl::RunCommands() {
    auto const list = TakeCommands();
    if (list.empty()) return;
    for (auto const& command : list) {
        if (!session) continue;
        try {
            switch (command.kind) {
                case Command::Kind::Toggle:
                    Await(session.TryTogglePlayPauseAsync());
                    break;
                case Command::Kind::Next:
                    Await(session.TrySkipNextAsync());
                    break;
                case Command::Kind::Prev:
                    Await(session.TrySkipPreviousAsync());
                    break;
                case Command::Kind::Seek: {
                    double const seconds = command.arg < 0 ? 0 : command.arg;
                    TimeSpan const target{(int64_t)(seconds * kTicksPerSecond)};
                    Await(session.TryChangePlaybackPositionAsync(target.count()));
                    break;
                }
            }
        } catch (...) {
            // アプリ側が要求を受け付けない場合がある。次の更新で追いつく。
            errlog::Exception(L"操作を送れませんでした",
                              Describe(std::current_exception()));
        }
    }
    // 操作直後は状態が変わるので少し待ってから読み直す
    std::this_thread::sleep_for(kCommandSettle);
}

void MediaController::Impl::OpenManager() {
    manager = Await(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());

    auto wake = [this](auto const&, auto const&) { Wake(); };
    try {
        manager_sessions_token = manager.SessionsChanged(wake);
    } catch (winrt::hresult_error const&) {
    }
    try {
        manager_current_token = manager.CurrentSessionChanged(wake);
    } catch (winrt::hresult_error const&) {
    }
}

void MediaController::Impl::DropManager() {
    DetachSession();
    if (manager) {
        try {
            if (manager_sessions_token) manager.SessionsChanged(manager_sessions_token);
        } catch (...) {
        }
        try {
            if (manager_current_token) {
                manager.CurrentSessionChanged(manager_current_token);
            }
        } catch (...) {
        }
    }
    manager_sessions_token = {};
    manager_current_token = {};
    manager = nullptr;
    track_key.clear();
    timeline.reset();
    unreadable = 0;
    ForgetArt();
}

void MediaController::Impl::AttachSession(
    GlobalSystemMediaTransportControlsSession const& next) {
    DetachSession();
    session = next;
    if (!session) return;

    auto wake = [this](auto const&, auto const&) { Wake(); };
    try {
        session_media_token = session.MediaPropertiesChanged(wake);
    } catch (winrt::hresult_error const&) {
    }
    try {
        session_playback_token = session.PlaybackInfoChanged(wake);
    } catch (winrt::hresult_error const&) {
    }
    try {
        session_timeline_token = session.TimelinePropertiesChanged(wake);
    } catch (winrt::hresult_error const&) {
    }
}

void MediaController::Impl::DetachSession() {
    if (session) {
        try {
            if (session_media_token) session.MediaPropertiesChanged(session_media_token);
        } catch (...) {
        }
        try {
            if (session_playback_token) {
                session.PlaybackInfoChanged(session_playback_token);
            }
        } catch (...) {
        }
        try {
            if (session_timeline_token) {
                session.TimelinePropertiesChanged(session_timeline_token);
            }
        } catch (...) {
        }
    }
    session_media_token = {};
    session_playback_token = {};
    session_timeline_token = {};
    session = nullptr;
}

void MediaController::Impl::NoteFailure(std::wstring const& reason,
                                        std::exception_ptr error) {
    // ここで取りこぼすとワーカー スレッドごと死ぬ。そうなると表示は止まった
    // まま、ボタンも効かないまま二度と戻らない (実際にそうなった)。相手の
    // アプリは曲を切り替えるたびにセッションを作り直すので、閉じかけの
    // セッションを触って想定外の失敗が返ってくることがある。何が起きても監視は
    // 続け、続けて転ぶようならセッションごと張り直す。
    ++failures;
    wchar_t count[32];
    swprintf(count, 32, L" (%d 回目)", failures);
    errlog::Exception(reason + count, Describe(error));
    if (failures >= kResetAfter) {
        failures = 0;
        errlog::Write(L"セッションを張り直します");
        DropManager();
    }
}

void MediaController::Impl::SkipUnreadable() {
    // 閉じかけのセッションでは GetPlaybackInfo() が null を返してくる (実測。
    // v13 のログには 1 日で 5 回残っていた)。失敗にして周回ごと転ばせると、
    // 3 回続いたときにセッションを張り直すことになり、そのときアルバム アートも
    // 覚えている再生位置も捨ててしまう。捨てた直後に Firefox の空タイムラインが
    // 来ると、せっかく直したバーがまた頭に戻る。
    //
    // 読めないだけなら、その周回を飛ばして次に賭ければよい。表示は 1 秒ぶん
    // 古いままになるだけ。ただし死んだセッションを掴んだまま二度と読めない目も
    // あるので、続くようなら張り直す。
    ++unreadable;
    if (unreadable >= kUnreadableLimit) {
        unreadable = 0;
        errlog::Write(L"再生情報を読めません。セッションを張り直します");
        DropManager();
    }
}

// --------------------------------------------------------------- セッション選択

std::pair<GlobalSystemMediaTransportControlsSession,
          std::vector<std::pair<std::wstring, std::wstring>>>
MediaController::Impl::PickSession() {
    std::vector<GlobalSystemMediaTransportControlsSession> sessions;
    std::vector<std::pair<std::wstring, std::wstring>> listing;
    for (auto const& s : manager.GetSessions()) {
        std::wstring id = AppIdOf(s);
        if (id.empty()) continue;
        sessions.push_back(s);
        listing.emplace_back(id, FriendlyAppName(id));
    }

    if (auto preferred = Preferred(); preferred && !preferred->empty()) {
        for (auto const& s : sessions) {
            if (AppIdOf(s) == *preferred) return {s, listing};
        }
    }

    auto is_playing = [](GlobalSystemMediaTransportControlsSession const& s) {
        try {
            auto info = s.GetPlaybackInfo();
            // 閉じかけのセッションは null を返してくる (実測)
            return info && info.PlaybackStatus() ==
                               GlobalSystemMediaTransportControlsSessionPlaybackStatus::
                                   Playing;
        } catch (winrt::hresult_error const&) {
            return false;
        }
    };

    GlobalSystemMediaTransportControlsSession current{nullptr};
    try {
        current = manager.GetCurrentSession();
    } catch (winrt::hresult_error const&) {
    }
    if (current && is_playing(current)) return {current, listing};
    for (auto const& s : sessions) {
        if (is_playing(s)) return {s, listing};
    }
    if (current) return {current, listing};
    return {sessions.empty() ? GlobalSystemMediaTransportControlsSession{nullptr}
                             : sessions.front(),
            listing};
}

// --------------------------------------------------------------- 状態取得

namespace {

/// タイムラインが最後に更新されてからの秒数。当てにならなければ 0。
///
/// LastUpdatedTime の中身は相手のアプリ任せで、桁の壊れた値が来ることがある。
/// 素で引き算すると監視ごと道連れになりかねないので、範囲で受け止める。
double ElapsedSince(DateTime updated) {
    if (updated.time_since_epoch().count() == 0) return 0.0;
    auto const now = winrt::clock::now();
    double const elapsed =
        std::chrono::duration_cast<std::chrono::duration<double>>(now - updated).count();
    return (elapsed >= 0.0 && elapsed < 3600.0) ? elapsed : 0.0;
}

Status ToStatus(GlobalSystemMediaTransportControlsSessionPlaybackStatus value) {
    using S = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    if (value == S::Playing) return Status::Playing;
    if (value == S::Paused) return Status::Paused;
    return Status::Stopped;
}

}  // namespace

void MediaController::Impl::Update() {
    if (!manager) return;

    auto [next, listing] = PickSession();
    // アプリ ID ではなくセッションそのものを見比べる。同じアプリでも曲を変えると
    // セッションは作り直されるので、ID で見ていると閉じた方を掴んだままになり、
    // イベントも届かず、ボタンの操作も宛先を失う。
    if (!next || !session || next != session) AttachSession(next);

    if (!session) {
        track_key.clear();
        timeline.reset();
        ForgetArt();
        auto snapshot = std::make_shared<NowPlaying>();
        snapshot->sessions = std::move(listing);
        snapshot->captured_at = NowMs();
        Publish(std::move(snapshot));
        return;
    }

    auto info = session.GetPlaybackInfo();
    if (!info) {
        SkipUnreadable();
        return;
    }
    unreadable = 0;
    Status const status = ToStatus(info.PlaybackStatus());
    auto const controls = info.Controls();

    double duration = 0.0;
    double position = 0.0;
    auto tl = session.GetTimelineProperties();
    // ここも null が返ることがある。長さも位置も分からないだけなので 0 のまま
    // 先へ進め、SteadyTimeline に直前の値を継がせる。
    if (tl) {
        double const start = tl.StartTime().count() / kTicksPerSecond;
        duration = tl.EndTime().count() / kTicksPerSecond - start;
        position = tl.Position().count() / kTicksPerSecond - start;
        if (duration < 0) duration = 0;
        if (position < 0) position = 0;
        if (status == Status::Playing) position += ElapsedSince(tl.LastUpdatedTime());
        if (duration > 0 && position > duration) position = duration;
    }

    auto props = Await(session.TryGetMediaPropertiesAsync());
    std::wstring title, artist, album;
    if (props) {
        title = HStr(props.Title());
        artist = HStr(props.Artist());
        if (artist.empty()) artist = HStr(props.AlbumArtist());
        album = HStr(props.AlbumTitle());
    }
    std::wstring const app_id = AppIdOf(session);
    std::wstring const key = app_id + L" | " + title + L" | " + artist + L" | " + album;

    // 曲名も演者も盤名も無い、名無しの間はアートを受け取らない。曲を切り替える
    // とき、ブラウザーは次の曲の名前が決まるまでの 1 秒足らずの間、自分のアプリ
    // アイコンをアートとして渡してくる (実測。Chrome の YouTube Music では
    // 256x256 の Chrome ロゴが来る)。素直に受け取ると曲が変わるたびにブラウザーの
    // ロゴが一瞬出て、そのあとアートに変わる。名無しの間は前の曲のアートを出した
    // ままにしておく方が落ち着く。
    bool const named = !(title.empty() && artist.empty() && album.empty());
    if (key != track_key) {
        // 曲が変わった。アートはすぐには追いついてこないので、しばらく読み直す。
        // 実測では 0.1〜0.5 秒ほど遅れて本来の画像が届く。
        track_key = key;
        art_deadline = NowMs() + (uint64_t)kArtRetryWindow.count();
        if (named && props) RefreshArt(props, true);
    } else if (named && props && NowMs() < art_deadline) {
        RefreshArt(props, false);
    }

    auto const steady = SteadyTimeline(key, status, duration, position);

    auto snapshot = std::make_shared<NowPlaying>();
    snapshot->app_id = app_id;
    snapshot->title = title;
    snapshot->artist = artist;
    snapshot->album = album;
    snapshot->status = status;
    snapshot->duration = steady.first;
    snapshot->position = steady.second;
    snapshot->captured_at = NowMs();
    snapshot->can_play = controls.IsPlayEnabled();
    snapshot->can_pause = controls.IsPauseEnabled();
    snapshot->can_next = controls.IsNextEnabled();
    snapshot->can_prev = controls.IsPreviousEnabled();
    snapshot->can_seek = controls.IsPlaybackPositionEnabled();
    snapshot->thumbnail = thumbnail;
    snapshot->art_serial = art_serial;
    snapshot->track_key = key;
    snapshot->sessions = std::move(listing);
    Publish(std::move(snapshot));
}

std::pair<double, double> MediaController::Impl::SteadyTimeline(std::wstring const& key,
                                                               Status status,
                                                               double duration,
                                                               double position) {
    // 空っぽのタイムラインで、いままでの再生位置を消さないようにする。
    //
    // Firefox は Google からログアウトした状態の YouTube / YouTube Music で
    // シークすると、再生を続けたまま「長さも位置も 0」のタイムラインを送って
    // くる (実測。しかも LastUpdatedTime だけは今の時刻で更新されるので、古い
    // 報せとして見分けることもできない)。素直に受け取ると曲の途中なのにバーが
    // 頭に戻り、一時停止して再生し直すまで戻らない。
    //
    // 長さの分かっている曲でそれが来たら、届かなかったものと見なして直前の値を
    // 進め続ける。長さの分からない曲 (ライブ配信など) は元から 0 なので通す。
    uint64_t const now = NowMs();
    if (duration > 0) {
        timeline = Timeline{key, duration, position, now};
        return {duration, position};
    }

    if (!timeline || timeline->key != key) {
        timeline.reset();
        return {duration, position};
    }

    double held = timeline->position;
    if (status == Status::Playing) {
        double const elapsed = (double)(now - timeline->at) / 1000.0;
        if (elapsed > 0) held += elapsed;
    }
    if (held > timeline->duration) held = timeline->duration;
    timeline = Timeline{key, timeline->duration, held, now};
    return {timeline->duration, held};
}

// --------------------------------------------------------------- アート

void MediaController::Impl::ForgetArt() {
    if (thumbnail) ++art_serial;
    thumbnail.reset();
    art_deadline = 0;
    art_pixels = 0;
    art_print.reset();
}

void MediaController::Impl::RefreshArt(
    GlobalSystemMediaTransportControlsSessionMediaProperties const& props, bool fresh) {
    // 同じ絵が 256x256 -> 150x150 -> 120x120 と、だんだん粗くなりながら届く
    // アプリがある。届いた順に採ると、小窓のアートが一瞬だけキレイですぐぼやけ、
    // 読み直しの時間が過ぎるとその粗いままで固定されてしまう。そこで、いま
    // 持っているものと同じ絵で、かつ小さいものは見送る。絵柄そのものが変わった
    // ときは、小さくなっていても素直に差し替える (見送ると前の曲のアートを
    // 出し続けることになってしまう)。
    auto latest = ReadThumbnail(props);
    if (!latest) {
        if (fresh) {  // アートの無い曲に変わった
            thumbnail.reset();
            art_pixels = 0;
            art_print.reset();
            ++art_serial;
        }
        return;
    }
    if (thumbnail && *thumbnail == *latest) return;

    int64_t pixels = 0;
    if (auto size = image::DecodeSize(*latest)) pixels = (int64_t)size->cx * size->cy;
    auto print = artwork::Fingerprint(*latest);
    if (pixels < art_pixels && artwork::SamePicture(print, art_print)) return;

    thumbnail = std::make_shared<std::vector<uint8_t> const>(std::move(*latest));
    art_pixels = pixels;
    art_print = std::move(print);
    ++art_serial;
}

std::optional<std::vector<uint8_t>> MediaController::Impl::ReadThumbnail(
    GlobalSystemMediaTransportControlsSessionMediaProperties const& props) {
    try {
        auto ref = props.Thumbnail();
        if (!ref) return std::nullopt;
        auto stream = Await(ref.OpenReadAsync());
        if (!stream) return std::nullopt;
        uint64_t const size = stream.Size();
        if (size == 0 || size > 32ull * 1024 * 1024) {
            stream.Close();
            return std::nullopt;
        }
        Buffer buffer((uint32_t)size);
        Await(stream.ReadAsync(buffer, (uint32_t)size, InputStreamOptions::ReadAhead));
        stream.Close();

        uint32_t const length = buffer.Length();
        if (length == 0) return std::nullopt;
        auto reader = DataReader::FromBuffer(buffer);
        std::vector<uint8_t> bytes(length);
        reader.ReadBytes(bytes);
        return bytes;
    } catch (winrt::hresult_error const&) {
        return std::nullopt;
    } catch (Stalled const&) {
        return std::nullopt;
    }
}

void MediaController::Impl::Publish(std::shared_ptr<NowPlaying> snapshot) {
    std::shared_ptr<NowPlaying const> const shared = std::move(snapshot);
    {
        std::lock_guard<std::mutex> guard(state_lock);
        state = shared;
    }
    try {
        if (on_update) on_update(shared);
    } catch (...) {
        // UI 側の失敗でワーカーを落とさない
    }
}

// --------------------------------------------------------------- 外向きの API

MediaController::MediaController(UpdateHandler on_update)
    : impl_(std::make_unique<Impl>(std::move(on_update))) {}

MediaController::~MediaController() {
    Stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

void MediaController::Start() {
    impl_->thread = std::thread([impl = impl_.get()] { impl->ThreadMain(); });
}

void MediaController::Stop() {
    impl_->stopping = true;
    impl_->Wake();
}

void MediaController::TogglePlayPause() {
    impl_->Submit({Impl::Command::Kind::Toggle, 0.0});
}
void MediaController::NextTrack() { impl_->Submit({Impl::Command::Kind::Next, 0.0}); }
void MediaController::PreviousTrack() { impl_->Submit({Impl::Command::Kind::Prev, 0.0}); }
void MediaController::Seek(double seconds) {
    impl_->Submit({Impl::Command::Kind::Seek, seconds});
}

void MediaController::SelectSession(std::optional<std::wstring> const& app_id) {
    {
        std::lock_guard<std::mutex> guard(impl_->preferred_lock);
        impl_->preferred_app = app_id;
    }
    impl_->Wake();
}

std::shared_ptr<NowPlaying const> MediaController::State() const {
    std::lock_guard<std::mutex> guard(impl_->state_lock);
    return impl_->state;
}

}  // namespace media
