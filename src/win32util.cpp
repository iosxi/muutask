#include "win32util.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <iterator>
#include <map>

namespace win32util {
namespace {

WheelHook* g_wheel_hook = nullptr;

std::optional<RECT> RectOf(HWND hwnd) {
    if (!hwnd) return std::nullopt;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return std::nullopt;
    return rect;
}

/// 画面の一部をメモリへ写し取ったもの。
struct ScreenShot {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // b, g, r, x

    bool empty() const { return pixels.empty(); }
    Rgb At(int x, int y) const {
        uint8_t const* p = pixels.data() + ((size_t)y * width + x) * 4;
        return Rgb{p[2], p[1], p[0]};
    }
};

/// 画面の矩形を 1 回の BitBlt で読む。
ScreenShot Capture(int x, int y, int width, int height) {
    ScreenShot shot;
    if (width <= 0 || height <= 0) return shot;

    HDC screen = GetDC(nullptr);
    if (!screen) return shot;
    HDC mem = CreateCompatibleDC(screen);
    if (!mem) {
        ReleaseDC(nullptr, screen);
        return shot;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // 上から下へ
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        if (BitBlt(mem, 0, 0, width, height, screen, x, y, SRCCOPY)) {
            GdiFlush();
            shot.width = width;
            shot.height = height;
            shot.pixels.assign((uint8_t*)bits,
                               (uint8_t*)bits + (size_t)width * height * 4);
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return shot;
}

/// いちばん多い色を選ぶ。同数のときは先に見つかった方 (Python の max と同じ)。
std::optional<Rgb> MostCommon(std::vector<Rgb> const& colors) {
    if (colors.empty()) return std::nullopt;
    Rgb best = colors[0];
    size_t best_count = 0;
    for (Rgb candidate : colors) {
        size_t count = 0;
        for (Rgb other : colors)
            if (other == candidate) ++count;
        if (count > best_count) {
            best_count = count;
            best = candidate;
        }
    }
    return best;
}

}  // namespace

HWND Taskbar() { return FindWindowW(L"Shell_TrayWnd", nullptr); }

std::optional<RECT> TaskbarRect() {
    HWND hwnd = Taskbar();
    if (!hwnd || !IsWindowVisible(hwnd)) return std::nullopt;
    auto rect = RectOf(hwnd);
    if (!rect) return std::nullopt;
    if (rect->right - rect->left <= 0 || rect->bottom - rect->top <= 0) {
        return std::nullopt;
    }
    return rect;
}

std::optional<RECT> ChildRect(wchar_t const* class_name) {
    HWND taskbar = Taskbar();
    if (!taskbar) return std::nullopt;
    return RectOf(FindWindowExW(taskbar, nullptr, class_name, nullptr));
}

void MakeToolWindow(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
}

bool IsCovered(HWND hwnd, int x, int y) {
    POINT point{x, y};
    HWND top = WindowFromPoint(point);
    if (!top) return true;
    HWND root = GetAncestor(top, GA_ROOT);
    return (root ? root : top) != hwnd;
}

bool PointOnTaskbar(int x, int y) {
    POINT point{x, y};
    HWND top = WindowFromPoint(point);
    if (!top) return false;
    HWND root = GetAncestor(top, GA_ROOT);
    if (!root) root = top;
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, (int)std::size(cls));
    return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

void RaiseToTop(HWND hwnd) {
    UINT const flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, flags);
}

std::optional<DWORD> WindowBand(HWND hwnd) {
    using GetWindowBandFn = BOOL(WINAPI*)(HWND, PDWORD);
    static GetWindowBandFn get_band = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? (GetWindowBandFn)GetProcAddress(user32, "GetWindowBand")
                      : nullptr;
    }();
    if (!get_band) return std::nullopt;
    DWORD band = 0;
    if (!get_band(hwnd, &band)) return std::nullopt;
    return band;
}

bool ForegroundIsFullscreen() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    // デスクトップ・タスクバー・自分自身は数えない
    for (wchar_t const* skip : {L"Shell_TrayWnd", L"WorkerW", L"Progman", L"MuuTaskBar",
                                L"MuuTaskPopup", L"MuuTaskHidden"}) {
        if (wcscmp(cls, skip) == 0) return false;
    }
    auto rect = RectOf(hwnd);
    if (!rect) return false;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    RECT const& m = info.rcMonitor;
    return rect->left <= m.left && rect->top <= m.top && rect->right >= m.right &&
           rect->bottom >= m.bottom;
}

std::vector<Rgb> SampleRows(int x, int y, int width, int height) {
    ScreenShot const shot = Capture(x, y, width, height);
    if (shot.empty()) return {};

    std::vector<Rgb> rows;
    rows.reserve((size_t)height);
    for (int row = 0; row < height; ++row) {
        std::vector<Rgb> samples;
        samples.reserve(5);
        for (int i = 1; i < 6; ++i) samples.push_back(shot.At(width * i / 6, row));
        auto color = MostCommon(samples);
        if (!color) return {};  // 1 行でも読めなければ「読めなかった」とする
        rows.push_back(*color);
    }
    return rows;
}

std::optional<Rgb> SampleColor(int x, int y, int width, int height) {
    ScreenShot const shot = Capture(x, y, width, height);
    if (shot.empty()) return std::nullopt;

    std::vector<Rgb> samples;
    for (int i = 1; i < 6; ++i) samples.push_back(shot.At(width * i / 6, height / 2));
    for (int i = 1; i < 6; ++i) samples.push_back(shot.At(width * i / 6, height / 4));
    return MostCommon(samples);
}

void TrimWorkingSet() {
    // 起動時にしか触らないもの (WinRT の初期化、フォントやコーデックの読み込み)
    // がそのまま常駐し続けるので、立ち上がったところで一度返す。捨てるのは
    // 物理メモリ上の常駐分だけで、必要になれば読み直されるため動作に影響は無い。
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

void VolumeStep(bool up) {
    BYTE const vk = up ? VK_VOLUME_UP : VK_VOLUME_DOWN;
    keybd_event(vk, 0, 0, 0);
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

// ----------------------------------------------------------------- VolumeMeter

VolumeMeter::~VolumeMeter() { Forget(); }

void VolumeMeter::Forget() {
    if (!endpoint_) return;
    endpoint_->Release();
    endpoint_ = nullptr;
}

bool VolumeMeter::Acquire() {
    if (endpoint_) return true;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                (void**)&enumerator))) {
        return false;
    }
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    enumerator->Release();
    if (FAILED(hr)) return false;

    void* volume = nullptr;
    hr = device->Activate(__uuidof(::IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    device->Release();
    if (FAILED(hr)) return false;
    endpoint_ = (::IAudioEndpointVolume*)volume;
    return true;
}

std::optional<VolumeMeter::Reading> VolumeMeter::Read() {
    // 1 回目が失敗したら、既定デバイスが差し替わったものとして掴み直す。
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Acquire()) return std::nullopt;
        float level = 0.0f;
        BOOL muted = FALSE;
        if (SUCCEEDED(endpoint_->GetMasterVolumeLevelScalar(&level)) &&
            SUCCEEDED(endpoint_->GetMute(&muted))) {
            Reading reading;
            reading.percent = Clamp(RoundToInt(level * 100.0), 0, 100);
            reading.muted = muted != FALSE;
            return reading;
        }
        Forget();
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ WheelHook

WheelHook::~WheelHook() { Uninstall(); }

bool WheelHook::Install(Handler handler) {
    if (hook_) return true;
    handler_ = std::move(handler);
    g_wheel_hook = this;
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &WheelHook::Thunk, GetModuleHandleW(nullptr),
                              0);
    if (!hook_) g_wheel_hook = nullptr;
    return hook_ != nullptr;
}

void WheelHook::Uninstall() {
    if (!hook_) return;
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    if (g_wheel_hook == this) g_wheel_hook = nullptr;
}

LRESULT CALLBACK WheelHook::Thunk(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && wparam == WM_MOUSEWHEEL && g_wheel_hook &&
        g_wheel_hook->handler_) {
        auto const* info = (MSLLHOOKSTRUCT const*)lparam;
        // 回した量は mouseData の上位 16 ビットに符号付きで入っている
        int delta = (short)HIWORD(info->mouseData);
        if (g_wheel_hook->handler_(info->pt.x, info->pt.y, delta)) {
            return 1;  // 下のウィンドウには渡さない
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace win32util
