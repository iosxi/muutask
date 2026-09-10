#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------- 色

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;

    constexpr COLORREF ref() const { return RGB(r, g, b); }
    constexpr bool operator==(Rgb const& o) const {
        return r == o.r && g == o.g && b == o.b;
    }
};

/// "#rrggbb" を読む。読めなければ fallback。
Rgb ParseHexColor(std::wstring const& text, Rgb fallback);
/// "#rrggbb" に書き出す。
std::wstring FormatHexColor(Rgb c);
/// 2 色を t (0..1) で混ぜる。t=1 で b。
Rgb MixColor(Rgb a, Rgb b, double t);
/// 明るさ (0..1)。文字色を決めるのに使う。
double Luminance(Rgb c);

// --------------------------------------------------------------------- 文字列

std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);
std::wstring ToLower(std::wstring_view text);

// --------------------------------------------------------------------- その他

/// exe の置かれているフォルダー (末尾に区切り文字は付けない)。
/// 設定もログもここに置く — やめるときにフォルダーごと消せば痕跡が残らない。
std::wstring const& ExeDirectory();

/// 起動からの経過ミリ秒。時計の変更に影響されない単調増加の値。
inline uint64_t NowMs() { return GetTickCount64(); }

inline int RoundToInt(double v) {
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

template <typename T>
inline T Clamp(T v, T low, T high) {
    return v < low ? low : (v > high ? high : v);
}
