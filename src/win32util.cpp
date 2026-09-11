#include "win32util.h"

// mmdeviceapi.h を先に置くこと。functiondiscoverykeys_devpkey.h は
// DEFINE_PROPERTYKEY が定義済みである前提で書かれていて、単独で読むと
// 「型指定子がありません」で通らない (実測)。
#include <mmdeviceapi.h>

#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
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

namespace {

std::wstring ClassOf(HWND hwnd) {
    wchar_t name[64] = {};
    GetClassNameW(hwnd, name, (int)std::size(name));
    return name;
}

}  // namespace

TaskbarSpot TaskbarHit(int x, int y) {
    POINT point{x, y};
    HWND top = WindowFromPoint(point);
    if (!top) return TaskbarSpot::None;
    HWND root = GetAncestor(top, GA_ROOT);
    if (!root) root = top;
    std::wstring const root_class = ClassOf(root);
    if (root_class != L"Shell_TrayWnd" && root_class != L"Shell_SecondaryTrayWnd") {
        return TaskbarSpot::None;
    }

    // 当たったところから root まで親をたどり、途中の入れ物で部分を見分ける。
    // 時計や個々のトレイ アイコンのように、もっと深いものが返ることがある。
    for (HWND node = top; node && node != root; node = GetParent(node)) {
        std::wstring const name = ClassOf(node);
        if (name == L"TrayNotifyWnd") return TaskbarSpot::Tray;
        // Windows 11 は MSTaskSwWClass が直に返る。10 はその中の
        // MSTaskListWClass が返ることがある
        if (name == L"MSTaskSwWClass" || name == L"MSTaskListWClass") {
            return TaskbarSpot::AppButtons;
        }
    }
    return TaskbarSpot::Blank;
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

void VolumeToggleMute() {
    keybd_event(VK_VOLUME_MUTE, 0, 0, 0);
    keybd_event(VK_VOLUME_MUTE, 0, KEYEVENTF_KEYUP, 0);
}

void VolumeStep(bool up) {
    BYTE const vk = up ? VK_VOLUME_UP : VK_VOLUME_DOWN;
    keybd_event(vk, 0, 0, 0);
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

// --------------------------------------------------------------- 音声の出力先

namespace {

/// 既定の出力先を切り替えるための、文書化されていない COM クラス。
///
/// 呼ぶのは SetDefaultEndpoint だけだが、vtable の並びを合わせないと別の
/// メソッドを叩いてしまうので、前に並ぶものも同じ形で書いておく (中身は
/// 使わないので、引数の型は大きさの合う void* で足りる)。
struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig
    : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64,
                                                          PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, REFPROPERTYKEY,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, REFPROPERTYKEY,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

/// デバイスの表示名。読めなければ空。
std::wstring FriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return L"";
    std::wstring name;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    store->Release();
    return name;
}

/// デバイスの識別子。読めなければ空。
std::wstring DeviceId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw) return L"";
    std::wstring id = raw;
    CoTaskMemFree(raw);
    return id;
}

}  // namespace

std::vector<AudioOutput> AudioOutputs() {
    std::vector<AudioOutput> outputs;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) {
        return outputs;
    }

    std::wstring current;
    IMMDevice* preferred = nullptr;
    if (SUCCEEDED(
            enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &preferred))) {
        current = DeviceId(preferred);
        preferred->Release();
    }

    IMMDeviceCollection* all = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) {
        UINT count = 0;
        all->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(all->Item(i, &device))) continue;
            AudioOutput output;
            output.id = DeviceId(device);
            output.name = FriendlyName(device);
            device->Release();
            if (output.id.empty()) continue;
            if (output.name.empty()) output.name = L"(名前の無い出力先)";
            output.current = !current.empty() && output.id == current;
            outputs.push_back(std::move(output));
        }
        all->Release();
    }
    enumerator->Release();
    return outputs;
}

bool SetDefaultAudioOutput(std::wstring const& id) {
    if (id.empty()) return false;
    IPolicyConfig* policy = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                __uuidof(IPolicyConfig), (void**)&policy))) {
        return false;
    }
    // 「既定のサウンド デバイス」は console と multimedia の 2 つ。
    // eCommunications は触らない (通話用の選択を壊さないため)。
    HRESULT const console = policy->SetDefaultEndpoint(id.c_str(), eConsole);
    HRESULT const multimedia = policy->SetDefaultEndpoint(id.c_str(), eMultimedia);
    policy->Release();
    return SUCCEEDED(console) && SUCCEEDED(multimedia);
}

// ----------------------------------------------------------------- VolumeMeter

/// 既定の出力先が変わったら印を立てるだけの係。
///
/// 呼び出しは MMDevice 側のスレッドから来るので、やることは 1 つの旗を
/// 立てるだけに留める。旗は Read() が見て、掴み直しの合図にする。
class VolumeMeter::Watcher : public IMMNotificationClient {
public:
    bool TakeStale() { return stale_.exchange(false); }

    // --- IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG const left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    // --- IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override {
        // 再生側の「既定」が動いたときだけ。通話用 (eCommunications) は見ない
        if (flow == eRender && (role == eConsole || role == eMultimedia)) {
            stale_ = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, PROPERTYKEY) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> stale_{false};
};

VolumeMeter::~VolumeMeter() { Forget(); }

void VolumeMeter::Forget() {
    if (endpoint_) {
        endpoint_->Release();
        endpoint_ = nullptr;
    }
    if (enumerator_ && watcher_) {
        enumerator_->UnregisterEndpointNotificationCallback(watcher_);
    }
    if (watcher_) {
        watcher_->Release();
        watcher_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
}

bool VolumeMeter::Acquire() {
    if (endpoint_) return true;
    if (!enumerator_) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    (void**)&enumerator_))) {
            enumerator_ = nullptr;
            return false;
        }
        // 出力先の切り替えに付いていくための知らせ。張れなくても音量は読める
        // ので、失敗しても先へ進む。
        watcher_ = new Watcher();
        if (FAILED(enumerator_->RegisterEndpointNotificationCallback(watcher_))) {
            watcher_->Release();
            watcher_ = nullptr;
        }
    }

    IMMDevice* device = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
        return false;
    }
    void* volume = nullptr;
    HRESULT const hr = device->Activate(__uuidof(::IAudioEndpointVolume), CLSCTX_ALL,
                                        nullptr, &volume);
    device->Release();
    if (FAILED(hr)) return false;
    endpoint_ = (::IAudioEndpointVolume*)volume;
    return true;
}

std::optional<VolumeMeter::Reading> VolumeMeter::Read() {
    // 出力先が切り替わっていたら、掴み直してから読む。掴んだままだと古い
    // デバイスの音量を返し続けてしまう。
    if (watcher_ && watcher_->TakeStale() && endpoint_) {
        endpoint_->Release();
        endpoint_ = nullptr;
    }
    // 1 回目が失敗したら、デバイスが消えたものとして掴み直す。
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
        endpoint_->Release();
        endpoint_ = nullptr;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ WheelHook

WheelHook::~WheelHook() { Uninstall(); }

bool WheelHook::Install(Handler wheel, ClickHandler middle_click) {
    if (hook_) return true;
    wheel_ = std::move(wheel);
    middle_click_ = std::move(middle_click);
    eat_middle_up_ = false;
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
    WheelHook* self = g_wheel_hook;
    if (code == HC_ACTION && self) {
        auto const* info = (MSLLHOOKSTRUCT const*)lparam;
        if (wparam == WM_MOUSEWHEEL && self->wheel_) {
            // 回した量は mouseData の上位 16 ビットに符号付きで入っている
            int const delta = (short)HIWORD(info->mouseData);
            if (self->wheel_(info->pt.x, info->pt.y, delta)) {
                return 1;  // 下のウィンドウには渡さない
            }
        } else if (wparam == WM_MBUTTONDOWN && self->middle_click_) {
            if (self->middle_click_(info->pt.x, info->pt.y)) {
                // 対になる離した方も捨てる。押した方だけ消すと、下の
                // ウィンドウに「押していないのに離れた」が届く
                self->eat_middle_up_ = true;
                return 1;
            }
        } else if (wparam == WM_MBUTTONUP && self->eat_middle_up_) {
            self->eat_middle_up_ = false;
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace win32util
