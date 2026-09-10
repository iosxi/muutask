// MuuTask — いま再生中の曲をタスクバーに表示するミニ プレーヤー。
//
// Windows のメディア コントロール (GSMTC) から曲名・アーティスト・アルバム
// アート・再生位置を受け取り、タスクバーの空きスペースに重ねたバーへ映す。

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "app.h"
#include "config.h"
#include "theme.h"
#include "version.h"

namespace {

constexpr wchar_t kMutexName[] = L"MuuTask.SingleInstance";

/// 二重起動を防ぐ (名前付きミューテックス)。
/// ハンドルはプロセス終了まで開いたままにしておく。
bool AlreadyRunning() {
    HANDLE handle = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!handle) return false;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/// コマンド ラインに指定の語があるか。
bool HasArgument(wchar_t const* name) {
    int count = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < count && !found; ++i) found = wcscmp(argv[i], name) == 0;
    LocalFree(argv);
    return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // build.ps1 が同梱用の config.json を書き出すために使う。既定値を
    // Config の定義と二重管理にしないための隠しスイッチで、書いたら終わる。
    if (HasArgument(L"--emit-config")) {
        Config().Save();
        return 0;
    }

    if (AlreadyRunning()) return 0;

    // 高 DPI でぼやけないように。ウィンドウを作る前に呼ぶこと。
    theme::EnableDpiAwareness();

    // WIC (アルバム アートのデコード) が要る。メディア監視のスレッドは
    // そちらで別に MTA を張る。
    HRESULT const com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;

    int code = 1;
    {
        App app;
        if (app.Initialize(instance)) code = app.Run();
    }

    CoUninitialize();
    return code;
}
