#include "menu.h"

#include <vector>

#include "common.h"

namespace menu {
namespace {

enum : UINT {
    kIdNone = 0,
    kIdPlayPause = 1,
    kIdNext = 2,
    kIdPrev = 3,
    kIdHideWhenIdle = 10,
    kIdWheelVolume = 11,
    kIdQuit = 20,
    kIdAnchorBase = 100,
    kIdWidthBase = 200,
    kIdArtBase = 300,
    kIdSessionAuto = 1000,
    kIdSessionBase = 1001,
};

void AddItem(HMENU parent, UINT id, std::wstring const& label, bool checked = false,
             bool radio = false) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    info.fType = radio ? MFT_RADIOCHECK : MFT_STRING;
    info.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
    info.wID = id;
    info.dwTypeData = const_cast<wchar_t*>(label.c_str());
    InsertMenuItemW(parent, GetMenuItemCount(parent), TRUE, &info);
}

void AddSeparator(HMENU parent) {
    AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
}

void AddSubmenu(HMENU parent, HMENU child, std::wstring const& label) {
    AppendMenuW(parent, MF_POPUP | MF_STRING, (UINT_PTR)child, label.c_str());
}

std::wstring WithValue(wchar_t const* label, int value) {
    return std::wstring(label) + L" (" + std::to_wstring(value) + L")";
}

}  // namespace

Selection Show(HWND owner, int x, int y, Config const& config,
               media::NowPlaying const& state) {
    HMENU root = CreatePopupMenu();
    if (!root) return {};

    AddItem(root, kIdPlayPause, L"再生 / 一時停止");
    AddItem(root, kIdNext, L"次の曲");
    AddItem(root, kIdPrev, L"前の曲");
    AddSeparator(root);

    HMENU sources = CreatePopupMenu();
    AddItem(sources, kIdSessionAuto, L"自動 (再生中のアプリ)", !config.session.has_value(),
            true);
    for (size_t i = 0; i < state.sessions.size(); ++i) {
        bool const chosen = config.session && *config.session == state.sessions[i].first;
        AddItem(sources, (UINT)(kIdSessionBase + i), state.sessions[i].second, chosen,
                true);
    }
    AddSubmenu(root, sources, L"再生元");
    AddSeparator(root);

    HMENU positions = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kAnchors); ++i) {
        AddItem(positions, (UINT)(kIdAnchorBase + i), kAnchors[i].label,
                config.bar_anchor == kAnchors[i].value, true);
    }
    AddSubmenu(root, positions, L"表示位置");

    HMENU widths = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kWidths); ++i) {
        AddItem(widths, (UINT)(kIdWidthBase + i),
                WithValue(kWidths[i].label, kWidths[i].value),
                config.bar_width == kWidths[i].value, true);
    }
    AddSubmenu(root, widths, L"バーの幅");

    HMENU art_sizes = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kArtSizes); ++i) {
        AddItem(art_sizes, (UINT)(kIdArtBase + i),
                WithValue(kArtSizes[i].label, kArtSizes[i].value),
                config.popup_height == kArtSizes[i].value, true);
    }
    AddSubmenu(root, art_sizes, L"アルバム アートの大きさ");

    AddItem(root, kIdHideWhenIdle, L"再生中のときだけ表示", config.bar_hide_when_idle);
    AddItem(root, kIdWheelVolume, L"ホイールで音量を調整", config.bar_wheel_volume);
    AddSeparator(root);
    AddItem(root, kIdQuit, L"終了");

    // メニューの外を押したときに閉じるには、いったんこちらが前面に居る必要が
    // ある。閉じたあとの WM_NULL も要る (これが無いと 2 度目が開かないことがある)。
    SetForegroundWindow(owner);
    UINT const chosen = (UINT)TrackPopupMenu(
        root, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN, x, y, 0,
        owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(root);

    Selection selection;
    if (chosen == kIdNone) return selection;
    if (chosen == kIdPlayPause) {
        selection.kind = Selection::Kind::PlayPause;
    } else if (chosen == kIdNext) {
        selection.kind = Selection::Kind::Next;
    } else if (chosen == kIdPrev) {
        selection.kind = Selection::Kind::Prev;
    } else if (chosen == kIdHideWhenIdle) {
        selection.kind = Selection::Kind::HideWhenIdle;
    } else if (chosen == kIdWheelVolume) {
        selection.kind = Selection::Kind::WheelVolume;
    } else if (chosen == kIdQuit) {
        selection.kind = Selection::Kind::Quit;
    } else if (chosen == kIdSessionAuto) {
        selection.kind = Selection::Kind::Session;
        selection.session = std::nullopt;
    } else if (chosen >= kIdSessionBase &&
               chosen < kIdSessionBase + state.sessions.size()) {
        selection.kind = Selection::Kind::Session;
        selection.session = state.sessions[chosen - kIdSessionBase].first;
    } else if (chosen >= kIdAnchorBase && chosen < kIdAnchorBase + std::size(kAnchors)) {
        selection.kind = Selection::Kind::Anchor;
        selection.anchor = kAnchors[chosen - kIdAnchorBase].value;
    } else if (chosen >= kIdWidthBase && chosen < kIdWidthBase + std::size(kWidths)) {
        selection.kind = Selection::Kind::Width;
        selection.size = kWidths[chosen - kIdWidthBase].value;
    } else if (chosen >= kIdArtBase && chosen < kIdArtBase + std::size(kArtSizes)) {
        selection.kind = Selection::Kind::ArtSize;
        selection.size = kArtSizes[chosen - kIdArtBase].value;
    }
    return selection;
}

}  // namespace menu
