// MuuTask — いま再生中の曲をタスクバーに表示するミニ プレーヤー。
//
// Windows のメディア コントロール (GSMTC) から曲名・アーティスト・アルバム
// アート・再生位置を受け取り、タスクバーの空きスペースに重ねたバーへ映す。

#include <windows.h>
#include <objbase.h>

#include "app.h"
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

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
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
