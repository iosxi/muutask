#include "theme.h"

#include <windows.h>

namespace theme {
namespace {

constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";

Rgb const kDefaultAccent{0x4c, 0xc2, 0xff};

bool ReadDword(wchar_t const* path, wchar_t const* name, DWORD* out) {
    DWORD size = sizeof(DWORD);
    return RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, out,
                        &size) == ERROR_SUCCESS;
}

}  // namespace

bool IsDarkMode() {
    DWORD value = 0;
    // 読めない環境ではダークとして扱う (Python 版と同じ)
    if (!ReadDword(kPersonalizeKey, L"AppsUseLightTheme", &value)) return true;
    return value == 0;
}

bool IsSystemDarkMode() {
    DWORD value = 0;
    // 同じキーの別の値。読めない環境ではダークとして扱う (IsDarkMode と同じ)
    if (!ReadDword(kPersonalizeKey, L"SystemUsesLightTheme", &value)) return true;
    return value == 0;
}

Rgb AccentColor() {
    DWORD value = 0;
    if (!ReadDword(kDwmKey, L"AccentColor", &value)) return kDefaultAccent;
    // AccentColor は 0xAABBGGRR
    return Rgb{(uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
               (uint8_t)((value >> 16) & 0xFF)};
}

Palette Palette::Current() {
    Palette p;
    p.dark = IsDarkMode();
    Rgb accent = AccentColor();
    if (p.dark) {
        // 暗いアクセント カラーだと沈むので少し明るくする
        p.accent = MixColor(accent, Rgb{255, 255, 255}, 0.15);
        p.card = Rgb{0x1f, 0x1f, 0x22};
        p.muted = Rgb{0x8b, 0x8b, 0x92};
        p.track = Rgb{0x3a, 0x3a, 0x40};
    } else {
        p.accent = MixColor(accent, Rgb{0, 0, 0}, 0.1);
        p.card = Rgb{0xfb, 0xfb, 0xfd};
        p.muted = Rgb{0x76, 0x76, 0x7e};
        p.track = Rgb{0xdc, 0xdc, 0xe2};
    }
    return p;
}

void EnableDpiAwareness() {
    // PER_MONITOR_AWARE_V2。古い Windows のために順に落としていく。
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = (SetContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return;
    }
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        using SetAwarenessFn = HRESULT(WINAPI*)(int);
        auto fn = (SetAwarenessFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn && SUCCEEDED(fn(2))) return;
    }
    SetProcessDPIAware();
}

double UiScale() {
    UINT dpi = GetDpiForSystem();
    if (dpi == 0) return 1.0;
    double scale = dpi / 96.0;
    return scale < 1.0 ? 1.0 : scale;
}

void RoundWindowCorners(HWND hwnd, int width, int height, int radius) {
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
    if (!region) return;
    // 成功すると region の所有権は OS に移るので、こちらでは消さない
    if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
}

}  // namespace theme
