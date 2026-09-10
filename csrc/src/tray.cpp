#include "tray.h"

#include <shellapi.h>

#include <algorithm>
#include <vector>

#include "app.h"
#include "artwork.h"
#include "common.h"
#include "image.h"

namespace {

constexpr UINT kIconId = 1;

/// 乗算済みアルファを戻す。CreateIconIndirect は素のアルファを求める。
image::Bgra Unpremultiply(image::Bgra const& src) {
    image::Bgra out = src;
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        unsigned a = out.pixels[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            out.pixels[i + c] = (uint8_t)std::min(255u, out.pixels[i + c] * 255u / a);
        }
    }
    return out;
}

}  // namespace

Tray::~Tray() { Close(); }

bool Tray::Create(App* app, HWND owner) {
    app_ = app;
    owner_ = owner;
    palette_ = theme::Palette::Current();
    icon_ = MakeIcon();
    return AddIcon();
}

void Tray::Close() {
    if (added_) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner_;
        data.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }
    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

HICON Tray::MakeIcon() const {
    // 曲が変わってもアイコンは音符のまま。通知領域では 16x16 まで縮められるので、
    // アルバム アートを入れても潰れて何が写っているか分からない。ここは MuuTask の
    // 目印として音符を出し続け、いま何が鳴っているかはツールチップに任せる。
    image::Bgra const art =
        artwork::NoteIcon(kIconSize, palette_.accent, palette_.card, 10);
    image::Bgra const straight = Unpremultiply(art);

    HBITMAP color = image::CreateDib(straight);
    if (!color) return nullptr;
    // 32bpp のアルファを使うので、マスクは全面 0 でよい
    HBITMAP mask = CreateBitmap(kIconSize, kIconSize, 1, 1, nullptr);
    if (!mask) {
        DeleteObject(color);
        return nullptr;
    }
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

bool Tray::AddIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_APP + 2;
    data.hIcon = icon_;
    wcsncpy_s(data.szTip, tooltip_.empty() ? L"MuuTask" : tooltip_.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    return added_;
}

void Tray::OnTaskbarCreated() {
    added_ = false;
    AddIcon();
}

std::wstring Tray::Tooltip(media::NowPlaying const& state) {
    if (!state.has_media() || (state.title.empty() && state.artist.empty())) {
        return L"MuuTask — 再生中の音楽はありません";
    }
    std::wstring text = state.is_playing() ? L"▶ " : L"❚❚ ";
    if (!state.title.empty()) text += state.title;
    if (!state.artist.empty()) {
        if (!state.title.empty()) text += L" — ";
        text += state.artist;
    }
    std::wstring const app_name = state.AppName();
    if (!app_name.empty()) text += L"  (" + app_name + L")";
    if (text.size() > kTooltipLimit) text.resize(kTooltipLimit);
    return text;
}

void Tray::Update(media::NowPlaying const& state) {
    std::wstring const tooltip = Tooltip(state);
    if (tooltip == tooltip_) return;
    tooltip_ = tooltip;
    if (!added_) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kIconId;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::OnMessage(WPARAM wparam, LPARAM lparam) {
    if (LOWORD(wparam) != kIconId || !app_) return;
    switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
            // 既定の操作は再生 / 一時停止 (Python 版の default=True と同じ)
            app_->TogglePlayPause();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT cursor{};
            GetCursorPos(&cursor);
            app_->ShowContextMenu(cursor.x, cursor.y);
            break;
        }
        default:
            break;
    }
}
