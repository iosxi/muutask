#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Windows のグローバル メディア セッション (GSMTC) を監視・操作するワーカー。
//
// Spotify / ブラウザー / メディア プレーヤーなど、Windows のメディア コント
// ロール (音量ポップアップに出るあれ) に対応しているアプリの再生情報をまとめて
// 扱う。WinRT の呼び出しは専用スレッドで行い、更新は on_update コールバックで
// スナップショットとして通知する。コールバックはワーカー スレッドから呼ばれる
// ので、UI 側でマーシャリングすること。
namespace media {

enum class Status { None, Playing, Paused, Stopped };

/// ある瞬間の再生状態のスナップショット。作ったあとは書き換えない。
struct NowPlaying {
    std::wstring app_id;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    Status status = Status::None;
    double position = 0.0;
    double duration = 0.0;
    uint64_t captured_at = 0;  // NowMs() の値
    bool can_play = false;
    bool can_pause = false;
    bool can_next = false;
    bool can_prev = false;
    bool can_seek = false;
    std::shared_ptr<std::vector<uint8_t> const> thumbnail;
    /// アートが差し替わるたびに増える。描き直すかの判定に使う。
    uint64_t art_serial = 0;
    /// 曲が変わったかどうかの判定用
    std::wstring track_key;
    /// (app_id, 表示名)
    std::vector<std::pair<std::wstring, std::wstring>> sessions;

    bool has_media() const { return status != Status::None; }
    bool is_playing() const { return status == Status::Playing; }

    /// スナップショット取得からの経過を足した、いまの再生位置。
    double LivePosition() const;

    std::wstring AppName() const;
};

/// AUMID や実行ファイル名を、人が読める短い名前にする。
std::wstring FriendlyAppName(std::wstring const& app_id);

/// メディア セッションを監視し、操作を送るコントローラ。
class MediaController {
public:
    /// ワーカー スレッドから呼ばれる。
    using UpdateHandler = std::function<void(std::shared_ptr<NowPlaying const>)>;

    explicit MediaController(UpdateHandler on_update);
    ~MediaController();

    MediaController(MediaController const&) = delete;
    MediaController& operator=(MediaController const&) = delete;

    void Start();
    void Stop();

    void TogglePlayPause();
    void NextTrack();
    void PreviousTrack();
    void Seek(double seconds);

    /// 特定アプリのセッションに固定する。無指定で自動選択に戻す。
    void SelectSession(std::optional<std::wstring> const& app_id);

    std::shared_ptr<NowPlaying const> State() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace media
