#include "common.h"

#include <algorithm>

namespace {

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

}  // namespace

Rgb ParseHexColor(std::wstring const& text, Rgb fallback) {
    if (text.size() != 7 || text[0] != L'#') return fallback;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = HexDigit(text[i + 1]);
        if (v[i] < 0) return fallback;
    }
    return Rgb{(uint8_t)(v[0] * 16 + v[1]), (uint8_t)(v[2] * 16 + v[3]),
               (uint8_t)(v[4] * 16 + v[5])};
}

std::wstring FormatHexColor(Rgb c) {
    wchar_t buf[8];
    swprintf(buf, 8, L"#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

Rgb MixColor(Rgb a, Rgb b, double t) {
    auto mix = [t](uint8_t x, uint8_t y) {
        return (uint8_t)Clamp(RoundToInt(x + (y - x) * t), 0, 255);
    };
    return Rgb{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

double Luminance(Rgb c) {
    return (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0;
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), len);
    return out;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::wstring ToLower(std::wstring_view text) {
    std::wstring out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return out;
}

std::wstring const& ExeDirectory() {
    static std::wstring const dir = [] {
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
            if (n == 0) return std::wstring(L".");
            if (n < path.size()) {
                path.resize(n);
                break;
            }
            path.resize(path.size() * 2);
        }
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
    }();
    return dir;
}
