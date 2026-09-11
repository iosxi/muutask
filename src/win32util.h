#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

#include "common.h"

// <endpointvolume.h> の中身。音量を読むためだけに使うので、ヘッダーは
// win32util.cpp の中だけに閉じ込める (ここは bar.h 経由で広く読まれる)。
struct IAudioEndpointVolume;

// タスクバーに重ねて表示するための Win32 まわりの薄いラッパー。
namespace win32util {

HWND Taskbar();

/// タスクバー本体の矩形。自動的に隠れている場合は無し。
std::optional<RECT> TaskbarRect();

/// タスクバーの子ウィンドウ (通知領域など) の矩形。
std::optional<RECT> ChildRect(wchar_t const* class_name);

/// Alt+Tab に出さず、クリックしてもフォーカスを奪わないようにする。
void MakeToolWindow(HWND hwnd);

/// 指定座標で、そのウィンドウが他のウィンドウに覆われているか。
bool IsCovered(HWND hwnd, int x, int y);

/// その座標に見えているのがタスクバーか (副モニターのものも含む)。
///
/// 矩形では見ない。カーソルの下のウィンドウを引いて、その root が
/// タスクバーかどうかで判じる。こうすると重なりの判定が一緒に片付く —
/// スタート メニュー・ウィジェット・通知領域のオーバーフロー・タスクの
/// サムネイルはどれもタスクバーの子ではない別ウィンドウなので、自動的に
/// 「タスクバーではない」側に落ちる。自動的に隠れる設定で引っ込んでいる
/// ときも同じ。
///
/// 実測 (Windows 11、3072px 幅のタスクバー): スタート付近・アプリ ボタン列
/// (MSTaskSwWClass)・空きスペース・通知領域 (TrayNotifyWnd) のどこを指しても
/// root は Shell_TrayWnd で返る。副モニターのタスクバーだけ
/// Shell_SecondaryTrayWnd になる。
bool PointOnTaskbar(int x, int y);

/// 最前面グループの先頭へ入れ直す。
///
/// タスクバーをクリックするとタスクバー自身が持ち上がり、こちらが下に潜る。
/// 既に最前面のウィンドウに HWND_TOPMOST を指定しても並び替えは起きないので、
/// HWND_TOP を使う。
void RaiseToTop(HWND hwnd);

/// ウィンドウの z バンド。取れなければ無し。
///
/// GetWindowBand は文書化されていないが Windows 8 以降の user32 にある。
/// タスクバーは通常こちらと同じ 1 (ZBID_DEFAULT) だが、スタート メニューを
/// 開くと 6 (ZBID_IMMERSIVE_EDGY) へ上がり、**別のウィンドウが活性化される
/// まで下りてこない**。上のバンドへは SetWindowPos では入れず、SetWindowBand は
/// UIAccess 権限が無いと ERROR_ACCESS_DENIED で弾かれる (実測)。
std::optional<DWORD> WindowBand(HWND hwnd);

/// 全画面のアプリ (ゲームや動画) が前面にあるか。
bool ForegroundIsFullscreen();

/// 指定した矩形を縦になぞって、行ごとの色を返す。
///
/// Windows 11 のタスクバーは**上端の 1 行だけ明るい** (実測で #424242 対
/// 本体 #212121)。一色で塗りつぶすとこの筋が消えてしまうので、行ごとの色を
/// 読んでそのまま描き写せるようにする。
///
/// 画面は BitBlt で一度だけ読み、あとはメモリ上で数える。画面 DC への
/// GetPixel は 1 画素あたり 16.7 ms かかり、300 画素で 5.0 秒に達した (実測)。
/// BitBlt なら同じ 300 画素が 20 ms で、返る色は 1 画素ずつ読んだものと
/// 完全に一致する (上端のハイライト行も含めて突き合わせ済み)。
std::vector<Rgb> SampleRows(int x, int y, int width, int height);

/// 矩形の中をまばらに拾って、いちばん多い色を返す (行ごとに読めなかったとき用)。
std::optional<Rgb> SampleColor(int x, int y, int width, int height);

/// 使っていないページを OS に返して常駐量を削る。
void TrimWorkingSet();

/// システムの音量を 1 段 (2%) 動かす。
///
/// 音量デバイスを COM で掴まず、メディア キーを送るだけにしている。既定の
/// 再生デバイスの選び直しや複数デバイスの扱いを OS に任せられるうえ、
/// Windows 標準の音量表示 (OSD) もそのまま出るため。
void VolumeStep(bool up);

/// 既定の再生デバイスの音量を読む。
///
/// 動かす方 (VolumeStep) はメディア キーに任せたままで、ここは読むだけ。
/// トレイのアイコンに数字を出すのに要る。
///
/// 既定デバイスが差し替わると掴んでいるインターフェイスは古くなり、以後
/// 失敗し続ける。読めなかったら一度手放して掴み直す。
///
/// COM を使うので、CoInitializeEx を済ませたスレッドから呼ぶこと
/// (このアプリでは main の STA = メイン ループのスレッド)。
class VolumeMeter {
public:
    struct Reading {
        int percent = 0;      // 0..100
        bool muted = false;   // ミュート中
    };

    ~VolumeMeter();

    /// 読めなければ無し (音の出口が 1 つも無いときなど)。
    std::optional<Reading> Read();
    /// 掴んでいるものを手放す。
    void Forget();

private:
    bool Acquire();

    IAudioEndpointVolume* endpoint_ = nullptr;
};

/// カーソルが特定の場所にあるときだけ、ホイールを横取りする。
///
/// バーは WS_EX_NOACTIVATE でフォーカスを持たないので、WM_MOUSEWHEEL は黙って
/// いても届かない。Windows はホイールをフォーカスのあるウィンドウへ送り、
/// 「非アクティブ ウィンドウをホバー時にスクロール」が入っているときだけ
/// カーソルの下のウィンドウへ送る。その設定を切っている環境でも効くように、
/// 低レベル マウス フックで拾う。
///
/// フックの処理は入力の流れを止めるので、中では位置の判定と音量キーの送出しか
/// 行わない (時間がかかると Windows にフックを外される)。
class WheelHook {
public:
    /// handler(x, y, delta) が true を返すと、下のウィンドウへ流さない。
    using Handler = std::function<bool(int, int, int)>;

    ~WheelHook();

    /// フックを張る。張れなければ false (呼び出し元はあきらめる)。
    /// 低レベル フックは張ったスレッドのメッセージ ループで呼ばれるので、
    /// メイン ループを回しているスレッドから呼ぶこと。
    bool Install(Handler handler);
    void Uninstall();
    bool installed() const { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK Thunk(int code, WPARAM wparam, LPARAM lparam);

    HHOOK hook_ = nullptr;
    Handler handler_;
};

}  // namespace win32util
