#include "app.h"

#include <windowsx.h>

#include "common.h"
#include "errlog.h"
#include "menu.h"
#include "theme.h"
#include "win32util.h"

namespace {

constexpr wchar_t kClassName[] = L"MuuTaskHidden";

constexpr UINT WM_MUUTASK_STATE = WM_APP + 1;
constexpr UINT WM_MUUTASK_TRAY = WM_APP + 2;

enum : UINT_PTR {
    kTimerTick = 1,    // 再生位置の進み
    kTimerSync = 2,    // タスクバーへの追従
    kTimerScroll = 3,  // 文字送り
    kTimerTrim = 4,    // 常駐量の返却
};

constexpr UINT kTickInterval = 250;
// 長いと潜ったときの復帰が目立つ
constexpr UINT kSyncInterval = 250;
// 25 コマ/秒
constexpr UINT kScrollInterval = 40;
// 起動が落ち着いてから常駐量を削るまで
constexpr UINT kTrimDelay = 5'000;
// 以後の間引き
constexpr UINT kTrimInterval = 600'000;

UINT TaskbarCreatedMessage() {
    static UINT const message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

}  // namespace

App::App() = default;
App::~App() = default;

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;
    scale_ = theme::UiScale();
    config_ = Config::Load();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &App::WndProc;
    cls.hInstance = instance;
    cls.lpszClassName = kClassName;
    RegisterClassExW(&cls);

    // メニューの持ち主。バーは WS_EX_NOACTIVATE なので前面になれず、そのままでは
    // メニューの外を押しても閉じない。普通のウィンドウをひとつ隠して持っておく。
    hidden_ = CreateWindowExW(0, kClassName, L"MuuTask", WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, instance, this);
    if (!hidden_) return false;

    if (!bar_.Create(this, instance)) return false;
    if (!popup_.Create(this, instance)) return false;
    tray_.Create(this, hidden_);

    controller_ = std::make_unique<media::MediaController>(
        [this](std::shared_ptr<media::NowPlaying const>) {
            // ワーカー スレッドから。最新の 1 件だけ持てばよいので、値は運ばず
            // 「見に来い」とだけ伝える。
            if (hidden_) PostMessageW(hidden_, WM_MUUTASK_STATE, 0, 0);
        });

    if (config_.session) controller_->SelectSession(config_.session);
    controller_->Start();

    SetTimer(hidden_, kTimerTick, kTickInterval, nullptr);
    SetTimer(hidden_, kTimerSync, kSyncInterval, nullptr);
    SetTimer(hidden_, kTimerScroll, kScrollInterval, nullptr);
    SetTimer(hidden_, kTimerTrim, kTrimDelay, nullptr);

    bar_.Sync();
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}

// --------------------------------------------------------------------- 操作

void App::TogglePlayPause() { controller_->TogglePlayPause(); }
void App::NextTrack() { controller_->NextTrack(); }
void App::PreviousTrack() { controller_->PreviousTrack(); }

void App::SetAnchor(std::wstring const& anchor) {
    config_.bar_anchor = anchor;
    config_.Save();
    bar_.Sync();
}

void App::SetWidth(int width) {
    config_.bar_width = width;
    config_.Save();
    bar_.Sync();
}

void App::SetArtSize(int height) {
    // ホバーで出る小窓の大きさ。開いていれば新しい大きさで出し直す。
    config_.popup_height = height;
    config_.Save();
    bar_.RefreshPopup();
}

void App::ToggleHideWhenIdle() {
    config_.bar_hide_when_idle = !config_.bar_hide_when_idle;
    config_.Save();
    bar_.Sync();
}

void App::ToggleWheelVolume() {
    config_.bar_wheel_volume = !config_.bar_wheel_volume;
    config_.Save();
    bar_.ApplyWheelVolume();
}

void App::SelectSession(std::optional<std::wstring> const& app_id) {
    config_.session = app_id;
    config_.Save();
    controller_->SelectSession(app_id);
}

void App::Quit() {
    bar_.Close();
    popup_.Close();
    tray_.Close();
    if (controller_) controller_->Stop();
    PostQuitMessage(0);
}

void App::ShowContextMenu(int x, int y) {
    menu_open_ = true;
    menu::Selection const selection = menu::Show(hidden_, x, y, config_, *state_);
    menu_open_ = false;

    using Kind = menu::Selection::Kind;
    switch (selection.kind) {
        case Kind::PlayPause: TogglePlayPause(); break;
        case Kind::Next: NextTrack(); break;
        case Kind::Prev: PreviousTrack(); break;
        case Kind::Anchor: SetAnchor(selection.anchor); break;
        case Kind::Width: SetWidth(selection.size); break;
        case Kind::ArtSize: SetArtSize(selection.size); break;
        case Kind::Session: SelectSession(selection.session); break;
        case Kind::HideWhenIdle: ToggleHideWhenIdle(); break;
        case Kind::WheelVolume: ToggleWheelVolume(); break;
        case Kind::Quit: Quit(); break;
        case Kind::None: break;
    }
}

// --------------------------------------------------------------------- 受け取り

void App::OnStatePosted() {
    auto latest = controller_->State();
    if (!latest || latest == state_) return;
    state_ = latest;
    bar_.UpdateState(state_);
    tray_.Update(*state_);
}

// --------------------------------------------------------------------- 窓の手続き

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    App* self = nullptr;
    if (message == WM_NCCREATE) {
        auto const* create = (CREATESTRUCTW const*)lparam;
        self = (App*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->Handle(hwnd, message, wparam, lparam);
}

LRESULT App::Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == TaskbarCreatedMessage()) {
        tray_.OnTaskbarCreated();
        return 0;
    }

    switch (message) {
        case WM_MUUTASK_STATE:
            OnStatePosted();
            return 0;

        case WM_MUUTASK_TRAY:
            tray_.OnMessage(wparam, lparam);
            return 0;

        case WM_TIMER:
            switch (wparam) {
                case kTimerTick:
                    if (state_->is_playing() && bar_.visible()) bar_.UpdateProgress();
                    return 0;
                case kTimerSync:
                    bar_.Sync();
                    return 0;
                case kTimerScroll:
                    bar_.ScrollTick();
                    return 0;
                case kTimerTrim:
                    win32util::TrimWorkingSet();
                    // 1 回目は起動から少しあとに、以後は長い間隔で
                    SetTimer(hwnd, kTimerTrim, kTrimInterval, nullptr);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_ENDSESSION:
        case WM_CLOSE:
            Quit();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
