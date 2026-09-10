#include "popup.h"

#include <algorithm>

#include "app.h"
#include "artwork.h"
#include "common.h"
#include "win32util.h"

namespace {
constexpr wchar_t kClassName[] = L"MuuTaskPopup";
}

Popup::~Popup() { Close(); }

bool Popup::Create(App* app, HINSTANCE instance) {
    app_ = app;
    palette_ = theme::Palette::Current();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &Popup::WndProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClassName;
    RegisterClassExW(&cls);

    // クリックしても前に出ないように。バーと同じ扱いにする。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                            kClassName, L"MuuTask", WS_POPUP, 0, 0, 10, 10, nullptr,
                            nullptr, instance, this);
    return hwnd_ != nullptr;
}

void Popup::Close() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

int Popup::Limit() const {
    int const height = app_->config().popup_height ? app_->config().popup_height
                                                   : kDefaultHeight;
    return std::max(80, RoundToInt(height * app_->scale()));
}

void Popup::Show(media::NowPlaying const& state, RECT const& bar_rect) {
    if (!hwnd_ || !app_) return;

    double const scale = app_->scale();
    int const box = Limit();
    int const pad = std::max(4, RoundToInt(kPad * scale));
    int const art = box - pad * 2;
    if (art <= 0) return;

    std::wstring const key = state.track_key + L"|" + std::to_wstring(state.art_serial) +
                             L"|" + std::to_wstring(art);
    if (key != key_ || box_ != box) {
        key_ = key;
        box_ = box;
        static std::vector<uint8_t> const kNoArt;
        art_ = artwork::AlbumArt(state.thumbnail ? *state.thumbnail : kNoArt, art,
                                 std::max(4, art / 16), palette_.muted, palette_.track,
                                 true);
        size_ = SIZE{art_.width + pad * 2, art_.height + pad * 2};
        art_x_ = pad;
        art_y_ = pad;
    }

    int const win_w = (int)size_.cx;
    int const win_h = (int)size_.cy;
    auto taskbar = win32util::TaskbarRect();
    int const top = taskbar ? taskbar->top : bar_rect.top;
    int const gap = std::max(2, RoundToInt(kGap * scale));

    int const bar_width = bar_rect.right - bar_rect.left;
    int left = bar_rect.left + bar_width / 2 - win_w / 2;
    left = Clamp(left, 0, std::max(0, GetSystemMetrics(SM_CXSCREEN) - win_w));

    SetWindowPos(hwnd_, nullptr, left, top - gap - win_h, win_w, win_h,
                 SWP_NOACTIVATE | SWP_NOZORDER);

    if (!visible_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
    // 絵の形が変わると小窓の大きさも変わる。角はそのたびに丸め直す
    if (rounded_.cx != size_.cx || rounded_.cy != size_.cy) {
        theme::RoundWindowCorners(hwnd_, win_w, win_h, std::max(4, RoundToInt(8 * scale)));
        rounded_ = size_;
    }
    win32util::RaiseToTop(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Popup::Hide() {
    if (!visible_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

LRESULT CALLBACK Popup::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Popup* self = nullptr;
    if (message == WM_NCCREATE) {
        auto const* create = (CREATESTRUCTW const*)lparam;
        self = (Popup*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (Popup*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->Handle(hwnd, message, wparam, lparam);
}

LRESULT Popup::Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH brush = CreateSolidBrush(palette_.card.ref());
            FillRect(dc, &client, brush);
            DeleteObject(brush);
            if (!art_.empty()) image::AlphaBlit(dc, art_x_, art_y_, art_);
            EndPaint(hwnd, &ps);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
