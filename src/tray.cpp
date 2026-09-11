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

// 下地を敷かずに数字を置くときの色。タスクバーの明暗に背ける。
constexpr Rgb kOnDark{0xf5, 0xf5, 0xf7};
constexpr Rgb kOnLight{0x16, 0x16, 0x1a};
// ミュート中。明るい地でも暗い地でも「沈んでいる」と分かる中間の灰色
constexpr Rgb kMutedInk{0x8a, 0x8a, 0x92};

/// 通知領域がアイコンを出す一辺 (px)。96 DPI で 16、150% で 24。
int SmallIconSize() {
    int const size = GetSystemMetrics(SM_CXSMICON);
    return size >= 8 ? size : 16;
}

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
    // 音量を先に渡されていれば、その数字のアイコンで出る
    if (!icon_) RefreshIcon();
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
    // 出すのはシステムの音量 (0-100)。アルバム アートは入れない — 通知領域では
    // 16x16 まで縮められ、何が写っているか分からなくなるため。いま何が鳴って
    // いるかはツールチップに任せる。
    //
    // 数字には角丸の下地を敷かない。16x16 で 3 桁を読めるようにするには枠を
    // まるごと字に使う必要があり、下地を敷くとその内側にしか置けなくなる。
    // 代わりにタスクバーの明暗に背けた色で字だけを置く (時計と同じ見え方)。
    //
    // 音量が読めないとき (音の出口が 1 つも無いときなど) は音符に戻す。
    image::Bgra art;
    if (volume_) {
        Rgb const ink = volume_->muted ? kMutedInk
                                       : (system_dark_ ? kOnDark : kOnLight);
        // 通知領域が出す大きさで直に描く。音符と同じ 64 で描いてシェルに縮めて
        // もらうと、数字の輪郭が甘くなる (実測。42 と 100 ではっきり差が出た)。
        // 形の滑らかな音符と違い、細い画で出来ている字は縮小に弱い。
        art = artwork::NumberIcon(SmallIconSize(), std::to_wstring(volume_->percent),
                                  ink, std::nullopt, 0);
    } else {
        art = artwork::NoteIcon(kIconSize, palette_.accent, palette_.card, 10);
    }
    image::Bgra const straight = Unpremultiply(art);

    HBITMAP color = image::CreateDib(straight);
    if (!color) return nullptr;
    // 32bpp のアルファを使うので、マスクは全面 0 でよい
    HBITMAP mask = CreateBitmap(art.width, art.height, 1, 1, nullptr);
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

std::wstring Tray::Tooltip() const {
    std::wstring text;
    // アイコンの数字は 16x16 まで縮むので、正確な値はここでも読めるようにする
    if (volume_) {
        text = L"音量 " + std::to_wstring(volume_->percent) + L"%";
        if (volume_->muted) text += L" (ミュート中)";
        text += L"\r\n";
    }
    text += track_.empty() ? L"MuuTask" : track_;
    if (text.size() > kTooltipLimit) text.resize(kTooltipLimit);
    return text;
}

std::wstring Tray::TrackText(media::NowPlaying const& state) {
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
    return text;
}

void Tray::RefreshIcon() {
    // テーマはここで読み直す。明暗を切り替えたとき、次に音量が動いた時点で
    // 新しい色になる (起動し直さなくてよい)。
    palette_ = theme::Palette::Current();
    system_dark_ = theme::IsSystemDarkMode();

    HICON fresh = MakeIcon();
    if (!fresh) return;  // 作れなければ、今出ているものを使い続ける
    HICON const old = icon_;
    icon_ = fresh;

    if (added_) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner_;
        data.uID = kIconId;
        data.uFlags = NIF_ICON;
        data.hIcon = icon_;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    // 差し替えが済んでから捨てる
    if (old) DestroyIcon(old);
}

void Tray::RefreshTooltip() {
    std::wstring const tooltip = Tooltip();
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

void Tray::Update(media::NowPlaying const& state) {
    std::wstring track = TrackText(state);
    if (track == track_) return;
    track_ = std::move(track);
    RefreshTooltip();
}

void Tray::SetVolume(std::optional<win32util::VolumeMeter::Reading> const& volume) {
    bool const same =
        (!volume_ && !volume) ||
        (volume_ && volume && volume_->percent == volume->percent &&
         volume_->muted == volume->muted);
    if (same) return;
    volume_ = volume;
    RefreshIcon();
    RefreshTooltip();
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
