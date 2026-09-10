#pragma once

#include <optional>
#include <string>

// 設定の保存。
//
// レジストリには一切書かない。設定は exe と同じフォルダーの config.json だけ
// なので、やめるときはフォルダーごと削除すれば痕跡が残らない。
struct Config {
    std::optional<std::wstring> session;   // 固定するアプリ ID (無指定 = 自動)

    // タスクバーに重ねて出すバー
    std::wstring bar_anchor = L"tray";     // left / start / tray
    bool bar_hide_when_idle = false;       // 再生中のときだけ出す
    int bar_width = 340;                   // 96 DPI 基準の px
    int bar_gap = 8;                       // 通知領域との間隔
    std::optional<std::wstring> bar_bg;    // "#rrggbb" 固定したいとき
    bool bar_wheel_volume = true;          // バーの上でホイールを回して音量を上下
    int popup_height = 180;                // ホバーで出る小窓の長辺 (96 DPI 基準)

    /// 設定を読む。読めなければ既定値。v1 以前の %APPDATA% から引き継ぐ。
    static Config Load();

    /// 保存に失敗しても、その回の動作は続けられるようにする。
    /// 書き込み不可の場所 (Program Files 直下など) では黙って諦め、その回の
    /// 変更は効いたまま次回起動時に既定値へ戻る。
    void Save() const;

    /// 設定ファイルの場所 (exe と同じフォルダーの config.json)。
    static std::wstring Path();
};
