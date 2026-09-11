#include "bar.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

#include "app.h"
#include "artwork.h"
#include "errlog.h"
#include "menu.h"

namespace {

constexpr wchar_t kClassName[] = L"MuuTaskBar";
constexpr wchar_t kEmptyTitle[] = L"再生中の音楽はありません";

//: 文字送りの速さ (96 DPI 基準の px/秒)
constexpr double kScrollSpeed = 34.0;
//: 先頭に戻ったところで止まる時間 (ms)。頭を読ませてから流す
constexpr uint64_t kScrollPause = 1000;
//: 繰り返しの間に入れる空き
constexpr wchar_t kScrollGap[] = L"　　　　";

//: ホイール 1 ノッチ分の値。高分解能ホイールはこれより細かい値で刻んで
//: くるので、貯めてから使う
constexpr int kWheelDelta = WHEEL_DELTA;

constexpr UINT_PTR kTimerPopup = 1;

constexpr wchar_t const* kTextFamilies[] = {L"Yu Gothic UI", L"Meiryo UI", L"Segoe UI"};
constexpr wchar_t const* kIconFamilies[] = {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"};

// Segoe Fluent Icons / Segoe MDL2 Assets のコード ポイントと、その代替
constexpr wchar_t kMdl2Prev[] = L"";
constexpr wchar_t kMdl2Next[] = L"";
constexpr wchar_t kMdl2Play[] = L"";
constexpr wchar_t kMdl2Pause[] = L"";
constexpr wchar_t kPlainPrev[] = L"⏮";
constexpr wchar_t kPlainNext[] = L"⏭";
constexpr wchar_t kPlainPlay[] = L"▶";
constexpr wchar_t kPlainPause[] = L"⏸";

bool DebugEnabled() {
    static bool const on = GetEnvironmentVariableW(L"MUUTASK_DEBUG", nullptr, 0) != 0;
    return on;
}

void DebugLog(std::wstring const& message) {
    if (!DebugEnabled()) return;
    OutputDebugStringW((L"MuuTask: " + message + L"\n").c_str());
    // コンソールを持たないので、追いかけられるようにログにも残す
    errlog::Write(L"[debug] " + message);
}

int CALLBACK FoundFont(LOGFONTW const*, TEXTMETRICW const*, DWORD, LPARAM param) {
    *(bool*)param = true;
    return 0;
}

bool FontExists(wchar_t const* family) {
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(query.lfFaceName, family, _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    EnumFontFamiliesExW(dc, &query, FoundFont, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

std::wstring PickFont(wchar_t const* const* candidates, size_t count,
                      wchar_t const* fallback) {
    for (size_t i = 0; i < count; ++i) {
        if (FontExists(candidates[i])) return candidates[i];
    }
    return fallback;
}

HFONT MakeFont(std::wstring const& family, int pixels, bool bold) {
    return CreateFontW(-pixels, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family.c_str());
}

/// 太さ 2 の丸い端点を持つ線。Tk の capstyle=ROUND に合わせる。
HPEN MakeRoundPen(Rgb color, int width) {
    LOGBRUSH brush{BS_SOLID, color.ref(), 0};
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width,
                        &brush, 0, nullptr);
}

}  // namespace

Bar::~Bar() { Close(); }

// --------------------------------------------------------------------- 生成

bool Bar::Create(App* app, HINSTANCE instance) {
    app_ = app;

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &Bar::WndProc;
    cls.hInstance = instance;
    cls.hCursor = nullptr;  // WM_SETCURSOR で自前に決める
    cls.lpszClassName = kClassName;
    RegisterClassExW(&cls);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClassName, L"MuuTask",
        WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    text_family_ = PickFont(kTextFamilies, std::size(kTextFamilies), L"Segoe UI");
    icon_family_ = PickFont(kIconFamilies, std::size(kIconFamilies), L"Segoe UI Symbol");
    icon_is_mdl2_ = icon_family_ != L"Segoe UI Symbol";
    measure_ = CreateCompatibleDC(nullptr);

    colors_ = MakeColors(std::nullopt);
    scrolled_at_ = NowMs();
    ApplyWheelVolume();
    return true;
}

void Bar::Close() {
    // 終了時の後片付け。フックは張りっぱなしにしない。
    wheel_hook_.reset();
    ReleaseFonts();
    if (surface_) {
        SelectObject(surface_, surface_old_);
        DeleteDC(surface_);
        surface_ = nullptr;
    }
    if (surface_bitmap_) {
        DeleteObject(surface_bitmap_);
        surface_bitmap_ = nullptr;
    }
    if (measure_) {
        DeleteDC(measure_);
        measure_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

std::optional<RECT> Bar::geometry() const { return geometry_; }

// --------------------------------------------------------------------- 配色

Bar::Colors Bar::MakeColors(std::optional<Rgb> sampled) const {
    // タスクバーの地色から、なじむ文字色を決める。
    Rgb bg{0x1f, 0x1f, 0x22};
    if (app_ && app_->config().bar_bg) {
        bg = ParseHexColor(*app_->config().bar_bg, bg);
    } else if (sampled) {
        bg = *sampled;
    }

    Colors c{};
    c.bg = bg;
    if (Luminance(bg) < 0.45) {
        c.title = Rgb{0xff, 0xff, 0xff};
        c.artist = Rgb{0xbd, 0xbd, 0xc6};
        c.button = Rgb{0xe4, 0xe4, 0xea};
        c.hover = Rgb{0xff, 0xff, 0xff};
        c.disabled = Rgb{0x6a, 0x6a, 0x72};
        c.track = Rgb{0x55, 0x55, 0x5f};
        c.end = Rgb{0x9a, 0x9a, 0xa4};
        // 座は半分の濃さで敷くので、色そのものは濃いめにしておかないと座に
        // 見えない (Python 版は 50% の網掛けで同じ見え方を作っていた)
        c.chip = MixColor(bg, Rgb{255, 255, 255}, 0.42);
        c.chip_hover = MixColor(bg, Rgb{255, 255, 255}, 0.72);
    } else {
        c.title = Rgb{0x10, 0x10, 0x14};
        c.artist = Rgb{0x4a, 0x4a, 0x52};
        c.button = Rgb{0x26, 0x26, 0x2c};
        c.hover = Rgb{0x00, 0x00, 0x00};
        c.disabled = Rgb{0xa0, 0xa0, 0xa8};
        c.track = Rgb{0xb6, 0xb6, 0xbe};
        c.end = Rgb{0x7a, 0x7a, 0x84};
        c.chip = MixColor(bg, Rgb{0, 0, 0}, 0.30);
        c.chip_hover = MixColor(bg, Rgb{0, 0, 0}, 0.55);
    }
    return c;
}

void Bar::SampleBackground(int x, int y, int width, int height) {
    // バーを置く場所のタスクバー色を拾って、地色を合わせる。
    // 一色ではなく行ごとに拾う。タスクバーは上端に細いハイライトが入っていて、
    // 一色で塗るとそこだけ塗り潰されて浮いて見えるため。
    bg_rows_.clear();
    if (app_->config().bar_bg) {
        colors_ = MakeColors(std::nullopt);
        return;
    }

    auto rows = win32util::SampleRows(x, y, width, height);
    if (!rows.empty()) {
        bg_rows_ = std::move(rows);
        // 文字色は本体の色で決める (上端のハイライトに引きずられないように)
        colors_ = MakeColors(bg_rows_[bg_rows_.size() / 2]);
        return;
    }
    if (auto color = win32util::SampleColor(x, y, width, height)) {
        colors_ = MakeColors(color);
    }
}

// --------------------------------------------------------------------- 組み立て

void Bar::Build(int width, int height) {
    Layout& l = layout_;
    l.width = width;
    l.height = height;
    l.pad = std::max(4, RoundToInt(height * 0.12));
    l.art = height - l.pad * 2;
    l.text_x = l.pad + l.art + std::max(6, RoundToInt(height * 0.16));

    int const btn = std::max(20, RoundToInt(height * 0.60));
    l.icon_px = std::max(11, RoundToInt(height * 0.26));
    l.title_px = std::max(11, RoundToInt(height * 0.27));
    l.artist_px = std::max(10, RoundToInt(height * 0.22));

    int const right = width - l.pad;
    l.next_cx = right - btn / 2;
    l.play_cx = l.next_cx - btn;
    l.prev_cx = l.play_cx - btn;
    l.cy = height / 2;

    l.chip_h = std::max(18, RoundToInt(height * 0.56));
    l.chip_w = std::max(18, btn - std::max(2, RoundToInt(height * 0.06)));
    int const chips_left = l.prev_cx - l.chip_w / 2;

    // 文字はバーの右端まで使う (収まらない分は流れてボタンの裏を通る)。ただし
    // 「流すかどうか」はボタンの手前までで判定する。ここに収まっていれば動かさず
    // に全部読めるし、はみ出すなら流さないと末尾がボタンの裏に隠れたままになる。
    l.text_width =
        std::max(20, chips_left - std::max(4, RoundToInt(height * 0.08)) - l.text_x);
    l.title_y = RoundToInt(height * 0.06);
    l.artist_y = RoundToInt(height * 0.58);

    l.prog_y = RoundToInt(height * 0.50);
    l.prog_x0 = l.text_x;
    l.prog_x1 = l.prev_cx - btn / 2 - std::max(6, RoundToInt(height * 0.12));
    l.end_h = std::max(3, RoundToInt(height * 0.10));
    l.radius = std::max(3, RoundToInt(std::min(l.chip_w, l.chip_h) * 0.30));

    EnsureFonts();
    EnsureSurface(width, height);

    // 座は毎コマ作り直さず、大きさが変わったときだけ焼いておく
    image::Mask const mask = image::RoundedRectMask(l.chip_w, l.chip_h, l.radius);
    image::Mask half = mask;
    for (uint8_t& v : half) v = (uint8_t)(v * 128 / 255);  // 50% の半透明
    chip_normal_ = image::Premultiply(colors_.chip, half, l.chip_w, l.chip_h);
    chip_hover_ = image::Premultiply(colors_.chip_hover, half, l.chip_w, l.chip_h);

    art_key_.clear();       // 作り直したので再描画させる
    title_ = Line{};        // 文字も入れ直す
    artist_ = Line{};
    built_size_ = std::make_pair(width, height);
}

void Bar::EnsureFonts() {
    ReleaseFonts();
    f_title_ = MakeFont(text_family_, layout_.title_px, true);
    f_artist_ = MakeFont(text_family_, layout_.artist_px, false);
    f_icon_ = MakeFont(icon_family_, layout_.icon_px, false);
}

void Bar::ReleaseFonts() {
    for (HFONT* font : {&f_title_, &f_artist_, &f_icon_}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void Bar::EnsureSurface(int width, int height) {
    if (surface_ && surface_w_ == width && surface_h_ == height) return;
    if (surface_) {
        SelectObject(surface_, surface_old_);
        DeleteDC(surface_);
        surface_ = nullptr;
    }
    if (surface_bitmap_) {
        DeleteObject(surface_bitmap_);
        surface_bitmap_ = nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    surface_bitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    surface_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (surface_ && surface_bitmap_) {
        surface_old_ = SelectObject(surface_, surface_bitmap_);
        surface_w_ = width;
        surface_h_ = height;
    }
}

int Bar::MeasureText(HFONT font, std::wstring const& text) {
    if (!measure_ || !font || text.empty()) return 0;
    HGDIOBJ old = SelectObject(measure_, font);
    SIZE size{};
    GetTextExtentPoint32W(measure_, text.c_str(), (int)text.size(), &size);
    SelectObject(measure_, old);
    return size.cx;
}

// --------------------------------------------------------------------- 描画

void Bar::PaintRows(HDC dc, int x0, int x1, int height) {
    // タスクバーの行ごとの色をそのまま描き写す。これで上端のハイライトがバーの
    // 上でも続き、透けているように見える。拾えなかったとき (bar_bg 指定時など)
    // は一色で塗る。
    if (x1 <= x0) return;
    if (bg_rows_.empty()) {
        RECT rect{x0, 0, x1, height};
        HBRUSH brush = CreateSolidBrush(colors_.bg.ref());
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        return;
    }
    // 同じ色が続くところはまとめて塗る (ふつうは 2〜3 本になる)
    int start = 0;
    for (int y = 0; y <= height; ++y) {
        Rgb const current = bg_rows_[(size_t)std::min(y, (int)bg_rows_.size() - 1)];
        Rgb const first = bg_rows_[(size_t)std::min(start, (int)bg_rows_.size() - 1)];
        if (y == height || !(current == first)) {
            RECT rect{x0, start, x1, y};
            HBRUSH brush = CreateSolidBrush(first.ref());
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
            start = y;
        }
    }
}

void Bar::Render() {
    if (!surface_ || !built_size_) return;
    Layout const& l = layout_;
    HDC dc = surface_;

    // 1. 地色 (いちばん奥)
    PaintRows(dc, 0, l.width, l.height);

    // 2. 文字
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    struct {
        Line const& line;
        HFONT font;
        Rgb color;
        int y;
    } const rows[] = {
        {title_, f_title_, colors_.title, l.title_y},
        {artist_, f_artist_, colors_.artist, l.artist_y},
    };
    for (auto const& row : rows) {
        if (row.line.text.empty() || !row.font) continue;
        HGDIOBJ old = SelectObject(dc, row.font);
        SetTextColor(dc, row.color.ref());
        TextOutW(dc, l.text_x - RoundToInt(row.line.offset), row.y, row.line.text.c_str(),
                 (int)row.line.text.size());
        SelectObject(dc, old);
    }

    // 3. アルバム アートの領域を隠す覆い (流れてきた文字をここで消す)。
    //    ここも行ごとに塗らないと、この幅だけハイライトが途切れる
    PaintRows(dc, 0, l.text_x - 1, l.height);
    if (!art_image_.empty()) image::AlphaBlit(dc, l.pad, l.pad, art_image_);

    // 4. シーク バー。タイトルとアーティストの間、ボタンの手前で止める
    if (state_->has_media()) {
        HPEN track = MakeRoundPen(colors_.track, 2);
        HGDIOBJ old_pen = SelectObject(dc, track);
        MoveToEx(dc, l.prog_x0, l.prog_y, nullptr);
        LineTo(dc, l.prog_x1, l.prog_y);
        SelectObject(dc, old_pen);
        DeleteObject(track);

        // 終端が分かるように、右端に縦の目印を立てる
        HPEN end = MakeRoundPen(colors_.end, 2);
        old_pen = SelectObject(dc, end);
        MoveToEx(dc, l.prog_x1, l.prog_y - l.end_h, nullptr);
        LineTo(dc, l.prog_x1, l.prog_y + l.end_h);
        SelectObject(dc, old_pen);
        DeleteObject(end);

        if (state_->duration > 0) {
            double const ratio =
                Clamp(state_->LivePosition() / state_->duration, 0.0, 1.0);
            int const filled =
                std::max(l.prog_x0 + 1,
                         l.prog_x0 + RoundToInt((l.prog_x1 - l.prog_x0) * ratio));
            HPEN fill = MakeRoundPen(colors_.title, 2);
            old_pen = SelectObject(dc, fill);
            MoveToEx(dc, l.prog_x0, l.prog_y, nullptr);
            LineTo(dc, filled, l.prog_y);
            SelectObject(dc, old_pen);
            DeleteObject(fill);
        }
    }

    // 5. ボタン (座 + 記号)。文字より前面に出したいので最後に描く
    DrawButtons(dc);

    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Bar::DrawButtons(HDC dc) {
    Layout const& l = layout_;
    struct Entry {
        Button button;
        int cx;
        wchar_t const* glyph;
    };
    wchar_t const* play_glyph =
        state_->is_playing() ? (icon_is_mdl2_ ? kMdl2Pause : kPlainPause)
                             : (icon_is_mdl2_ ? kMdl2Play : kPlainPlay);
    Entry const entries[] = {
        {Button::Prev, l.prev_cx, icon_is_mdl2_ ? kMdl2Prev : kPlainPrev},
        {Button::Play, l.play_cx, play_glyph},
        {Button::Next, l.next_cx, icon_is_mdl2_ ? kMdl2Next : kPlainNext},
    };

    SetBkMode(dc, TRANSPARENT);
    for (Entry const& entry : entries) {
        bool const hovered = hover_ == entry.button;
        image::Bgra const& chip = hovered ? chip_hover_ : chip_normal_;
        image::AlphaBlit(dc, entry.cx - l.chip_w / 2, l.cy - l.chip_h / 2, chip);

        Rgb color = colors_.button;
        if (!Enabled(entry.button)) {
            color = colors_.disabled;
        } else if (hovered) {
            color = colors_.hover;
        }
        if (!f_icon_) continue;
        HGDIOBJ old = SelectObject(dc, f_icon_);
        SetTextColor(dc, color.ref());
        RECT box{entry.cx - l.chip_w, l.cy - l.chip_h, entry.cx + l.chip_w,
                 l.cy + l.chip_h};
        DrawTextW(dc, entry.glyph, -1, &box,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }
}

// --------------------------------------------------------------------- 状態

void Bar::UpdateState(std::shared_ptr<media::NowPlaying const> state) {
    state_ = std::move(state);
    if (!built_size_) return;

    media::NowPlaying const& s = *state_;
    bool const has_media = s.has_media() && !(s.title.empty() && s.artist.empty());

    SetText(L"title", has_media ? s.title : kEmptyTitle);
    // アプリ名は入れない (長くなるぶん無駄に流れる)。トレイのツールチップには出る
    std::wstring artist;
    if (has_media) {
        artist = s.artist;
        if (!s.album.empty()) {
            artist = artist.empty() ? s.album : artist + L" · " + s.album;
        }
    }
    SetText(L"artist", artist);

    std::wstring const key = s.track_key + L"|" + std::to_wstring(s.art_serial) + L"|" +
                             std::to_wstring((int)s.status) + L"|" +
                             std::to_wstring(layout_.art);
    if (key != art_key_) {
        art_key_ = key;
        static std::vector<uint8_t> const kNoArt;
        art_image_ = artwork::AlbumArt(s.thumbnail ? *s.thumbnail : kNoArt, layout_.art,
                                       std::max(2, layout_.art / 8), colors_.artist,
                                       colors_.track, false);
        if (has_media && !s.is_playing()) image::Dim(art_image_, 0.35);
    }

    Render();

    // 出したまま曲が変わったら、小窓のアートも差し替える
    if (app_ && app_->popup().visible() && geometry_) {
        app_->popup().Show(s, *geometry_);
    }
}

void Bar::UpdateProgress() {
    if (!built_size_ || !visible_) return;
    Render();
}

void Bar::SetText(wchar_t const* key, std::wstring const& text) {
    // 文字を入れる。入りきらないときは流せるように 2 つ繋げて持たせる。
    // 同じ文字なら位置を保つ (更新のたびに頭へ戻ると読めなくなる)。
    Line& line = wcscmp(key, L"title") == 0 ? title_ : artist_;
    HFONT const font = wcscmp(key, L"title") == 0 ? f_title_ : f_artist_;
    if (line.source == text) return;
    line.source = text;

    if (MeasureText(font, text) <= layout_.text_width) {
        line.period = 0;
        line.text = text;
    } else {
        // 末尾まで流れたら頭に戻る、を繋ぎ目なく見せるため 2 回並べる
        line.period = MeasureText(font, text + kScrollGap);
        line.text = text + kScrollGap + text;
    }
    line.offset = 0.0;
    line.pause_until = NowMs() + kScrollPause;
}

void Bar::ScrollTick() {
    // 先頭に戻ったところで少し止める。行ごとに独立して数えるので、タイトルと
    // アーティスト行の停止は揃わない (長さが違うため)。
    if (!visible_ || !built_size_) return;
    uint64_t const now = NowMs();
    uint64_t const elapsed_ms = now - scrolled_at_;
    scrolled_at_ = now;
    if (elapsed_ms == 0 || elapsed_ms > 1000) return;  // 復帰直後などに飛ばないように

    double const step = kScrollSpeed * (app_ ? app_->scale() : 1.0) * elapsed_ms / 1000.0;
    bool moved = false;
    for (Line* line : {&title_, &artist_}) {
        if (line->period <= 0 || now < line->pause_until) continue;
        double offset = line->offset + step;
        if (offset >= line->period) {
            // 一周した。先頭に戻して、また少し止める
            offset = 0.0;
            line->pause_until = now + kScrollPause;
        }
        line->offset = offset;
        moved = true;
    }
    if (moved) Render();
}

// --------------------------------------------------------------------- 入力

Bar::Button Bar::HitTest(int x, int y) const {
    if (!built_size_) return Button::None;
    Layout const& l = layout_;
    int const half = l.chip_w / 2;
    struct {
        Button button;
        int cx;
    } const boxes[] = {
        {Button::Prev, l.prev_cx}, {Button::Play, l.play_cx}, {Button::Next, l.next_cx}};
    for (auto const& box : boxes) {
        if (x >= box.cx - half && x <= box.cx + half && y >= 0 && y <= l.height) {
            return Enabled(box.button) ? box.button : Button::None;
        }
    }
    return Button::None;
}

bool Bar::Enabled(Button button) const {
    switch (button) {
        case Button::Prev: return state_->can_prev;
        case Button::Next: return state_->can_next;
        case Button::Play: return state_->can_play || state_->can_pause;
        default: return true;
    }
}

void Bar::SetHover(Button button) {
    if (button == hover_) return;
    hover_ = button;
    Render();
}

void Bar::ArmPopup() {
    // カーソルが乗っている間だけ、少し待ってから小窓を出す。
    if (!app_ || app_->popup().visible() || popup_armed_ || app_->menu_open()) return;
    SetTimer(hwnd_, kTimerPopup, Popup::kDelay, nullptr);
    popup_armed_ = true;
}

void Bar::CancelPopup() {
    if (!popup_armed_) return;
    KillTimer(hwnd_, kTimerPopup);
    popup_armed_ = false;
}

void Bar::OpenPopup() {
    CancelPopup();
    if (!app_ || !visible_ || !geometry_ || app_->menu_open()) return;
    app_->popup().Show(*state_, *geometry_);
}

void Bar::RefreshPopup() {
    if (app_ && app_->popup().visible() && geometry_) {
        app_->popup().Show(*state_, *geometry_);
    }
}

// --------------------------------------------------------- ホイールで音量

void Bar::ApplyWheelVolume() {
    // 低レベル フックは張ったスレッドで呼ばれるので、メイン ループを回している
    // スレッドから呼ぶこと。
    bool const want = app_ && app_->config().bar_wheel_volume;
    if (want && !wheel_hook_) {
        auto hook = std::make_unique<win32util::WheelHook>();
        if (hook->Install([this](int x, int y, int delta) { return OnWheel(x, y, delta); })) {
            wheel_hook_ = std::move(hook);
        }
    } else if (!want && wheel_hook_) {
        wheel_hook_.reset();
    }
    wheel_accum_ = 0;
}

bool Bar::WheelTarget(int x, int y) const {
    // 全画面のアプリでタスクバーが引っ込んでいる間は、何も横取りしない。
    // ゲームや全画面の動画で、視界の外の操作に音量が動くのを避ける。
    if (win32util::ForegroundIsFullscreen()) return false;

    // 通知領域もアプリ ボタン列も含めた、タスクバーの全体。矩形ではなく
    // 「その座標に見えているのがタスクバーか」で見ているので、スタート
    // メニューや各種フライアウトが開いていれば自動的にそちらの取り分になる。
    if (win32util::PointOnTaskbar(x, y)) return true;

    // バーはタスクバーの子ではなく、上に重ねた別ウィンドウなので別に見る。
    if (!visible_ || !geometry_) return false;
    RECT const& g = *geometry_;
    if (!(x >= g.left && x < g.right && y >= g.top && y < g.bottom)) return false;
    // 何かに覆われているなら、そのウィンドウの取り分
    return !win32util::IsCovered(hwnd_, x, y);
}

bool Bar::OnWheel(int x, int y, int delta) {
    // タスクバーの上で回されたホイールを音量に回す。true で下へ流さない。
    if (!WheelTarget(x, y)) {
        wheel_accum_ = 0;
        return false;
    }

    wheel_accum_ += delta;
    while (std::abs(wheel_accum_) >= kWheelDelta) {
        bool const up = wheel_accum_ > 0;
        wheel_accum_ += up ? -kWheelDelta : kWheelDelta;
        win32util::VolumeStep(up);
    }
    return true;
}

// --------------------------------------------------------------------- 位置合わせ

std::optional<std::pair<int, int>> Bar::Placement(RECT const& taskbar) const {
    // 設定に応じた (x, 幅) を返す。置ける場所がなければ無し。
    int const left = taskbar.left;
    int const right = taskbar.right;
    double const scale = app_->scale();

    auto notify = win32util::ChildRect(L"TrayNotifyWnd");
    int const tray_left = notify ? notify->left : right - RoundToInt(200 * scale);
    auto start = win32util::ChildRect(L"Start");
    int const start_left = start ? start->left : left;

    Config const& config = app_->config();
    int const gap = RoundToInt((config.bar_gap ? config.bar_gap : kDefaultGap) * scale);
    int width = RoundToInt((config.bar_width ? config.bar_width : kDefaultWidth) * scale);

    int region_right = 0;
    bool align_right = true;
    if (config.bar_anchor == L"start" && start_left - left > width) {
        // スタート ボタンのすぐ左に寄せる (アイコンが中央寄せのとき)
        region_right = start_left;
        align_right = true;
    } else if (config.bar_anchor == L"left") {
        // 左端。スタート ボタンが中央寄せなら、その手前までに収める
        region_right = (start_left - left >= width + gap * 2) ? start_left : tray_left;
        align_right = false;
    } else {
        region_right = tray_left;
        align_right = true;
    }

    width = std::min(width, region_right - left - gap * 2);
    if (width < RoundToInt(kMinWidth * scale)) {
        DebugLog(L"  置き場所が狭い: 幅 " + std::to_wstring(width) + L" (左 " +
                 std::to_wstring(left) + L" / 右端 " + std::to_wstring(region_right) +
                 L" / 通知領域 " + std::to_wstring(tray_left) + L" / スタート " +
                 std::to_wstring(start_left) + L")");
        return std::nullopt;
    }
    int const x = align_right ? region_right - gap - width : left + gap;
    return std::make_pair(x, width);
}

void Bar::Sync() {
    if (!app_) return;
    // ラベルどおり「再生中」だけを見る。has_media だと一時停止中のセッションが
    // 残っているアプリ (ブラウザーなど) で永久に隠れず、設定が効かなかった。
    // 隠す理由は、実際に消すときだけ残す (毎周回のログにしない)
    if (app_->config().bar_hide_when_idle && !state_->is_playing()) {
        Hide(L"再生中のときだけ表示");
        return;
    }
    auto taskbar = win32util::TaskbarRect();
    if (!taskbar) {
        Hide(L"タスクバーが見つからない");
        return;
    }
    if (win32util::ForegroundIsFullscreen()) {
        Hide(L"前面が全画面");
        return;
    }

    int const height = taskbar->bottom - taskbar->top;
    auto placement = Placement(*taskbar);
    if (!placement || height <= 0) {
        Hide(L"置ける場所がない");
        return;
    }
    int const x = placement->first;
    int const width = placement->second;
    int const y = taskbar->top;

    // 置き場所を変えたときは、その場所の地色を測り直してから作り直す
    std::wstring const& anchor = app_->config().bar_anchor;
    if (built_size_ != std::make_pair(width, height) || anchor_used_ != anchor) {
        if (visible_) {
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
            Sleep(50);  // 画面から消えるのを待ってから色を拾う
        }
        SampleBackground(x, y, width, height);
        anchor_used_ = anchor;
        Build(width, height);
        UpdateState(state_);
        DebugLog(L"組み立て直しました " + std::to_wstring(width) + L"x" +
                 std::to_wstring(height) + L" @(" + std::to_wstring(x) + L"," +
                 std::to_wstring(y) + L")");
    }

    RECT const geometry{x, y, x + width, y + height};
    if (!geometry_ || memcmp(&*geometry_, &geometry, sizeof(RECT)) != 0) {
        SetWindowPos(hwnd_, nullptr, x, y, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        geometry_ = geometry;
    }

    if (!visible_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
        Render();
    }

    KeepInFront(x + width / 2, y + height / 2);
}

void Bar::KeepInFront(int cx, int cy) {
    // タスクバーに潜られたら前に出し直す。
    //
    // ただし スタート メニューを開くと、タスクバーは z バンドごと上がり (1 -> 6)、
    // **スタート メニューを閉じただけでは下りてこない**。別のウィンドウが
    // 活性化されるまで上がったままで、その間はこちらからどうやっても前に出られ
    // ない (UIAccess 権限が要る)。越えられないと分かっている間は要求を出さない。
    if (!win32util::IsCovered(hwnd_, cx, cy)) {
        if (covered_since_) {
            DebugLog(L"復帰 (" + std::to_wstring((NowMs() - *covered_since_) / 1000.0) +
                     L"s)");
            covered_since_.reset();
        }
        return;
    }

    if (!covered_since_) {
        covered_since_ = NowMs();
        DebugLog(L"タスクバーに覆われました");
    }

    HWND taskbar = win32util::Taskbar();
    auto their_band = taskbar ? win32util::WindowBand(taskbar) : std::nullopt;
    auto our_band = win32util::WindowBand(hwnd_);
    if (their_band && our_band && *their_band > *our_band) {
        DebugLog(L"  バンドが上なので手が出せない");
        return;
    }
    win32util::RaiseToTop(hwnd_);
}

void Bar::Hide(wchar_t const* reason) {
    if (!visible_) return;
    if (reason) DebugLog(std::wstring(L"隠します: ") + reason);
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    SetHover(Button::None);
    CancelPopup();
    if (app_) app_->popup().Hide();
}

// --------------------------------------------------------------------- 窓の手続き

LRESULT CALLBACK Bar::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Bar* self = nullptr;
    if (message == WM_NCCREATE) {
        auto const* create = (CREATESTRUCTW const*)lparam;
        self = (Bar*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (Bar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->Handle(hwnd, message, wparam, lparam);
}

LRESULT Bar::Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;  // 全面を自前で描くので、下地を塗らせない (ちらつき防止)

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (surface_) {
                BitBlt(dc, 0, 0, surface_w_, surface_h_, surface_, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                tracking_mouse_ = true;
            }
            SetHover(HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)));
            ArmPopup();
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            SetHover(Button::None);
            CancelPopup();
            if (app_) app_->popup().Hide();
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, hover_ != Button::None ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN:
            pressed_ = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;

        case WM_LBUTTONUP: {
            Button const button = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            Button const pressed = pressed_;
            pressed_ = Button::None;
            if (button == Button::None || button != pressed || !app_) return 0;
            if (button == Button::Prev) app_->PreviousTrack();
            else if (button == Button::Next) app_->NextTrack();
            else if (button == Button::Play) app_->TogglePlayPause();
            return 0;
        }

        case WM_RBUTTONUP: {
            // メニューを出す前に小窓を消す。小窓は最前面なので、残しておくと
            // メニューに覆いかぶさってしまう
            CancelPopup();
            if (app_) {
                app_->popup().Hide();
                POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(hwnd, &screen);
                app_->ShowContextMenu(screen.x, screen.y);
            }
            return 0;
        }

        case WM_TIMER:
            if (wparam == kTimerPopup) {
                OpenPopup();
                return 0;
            }
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
