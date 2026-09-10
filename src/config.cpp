#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "common.h"

namespace {

// --------------------------------------------------------------- 小さな JSON

/// 設定ファイルは「値が null / 真偽 / 整数 / 文字列だけの平たいオブジェクト」に
/// 限られる。汎用の JSON ライブラリを抱き込む理由が無いので、その形だけを読む。
class FlatJson {
public:
    explicit FlatJson(std::wstring const& text) : text_(text) { Parse(); }

    bool HasKey(std::wstring const& key) const { return Find(key) != nullptr; }

    std::optional<std::wstring> String(std::wstring const& key) const {
        Entry const* e = Find(key);
        if (!e || e->kind != Kind::String) return std::nullopt;
        return e->text;
    }

    std::optional<bool> Bool(std::wstring const& key) const {
        Entry const* e = Find(key);
        if (!e || e->kind != Kind::Bool) return std::nullopt;
        return e->boolean;
    }

    std::optional<int> Int(std::wstring const& key) const {
        Entry const* e = Find(key);
        if (!e || e->kind != Kind::Number) return std::nullopt;
        return e->number;
    }

    /// キーはあるが値が null。「未設定」と「読めなかった」を区別するのに使う。
    bool IsNull(std::wstring const& key) const {
        Entry const* e = Find(key);
        return e && e->kind == Kind::Null;
    }

private:
    enum class Kind { Null, Bool, Number, String };
    struct Entry {
        std::wstring key;
        Kind kind = Kind::Null;
        std::wstring text;
        bool boolean = false;
        int number = 0;
    };

    Entry const* Find(std::wstring const& key) const {
        for (auto const& e : entries_)
            if (e.key == key) return &e;
        return nullptr;
    }

    void SkipSpace() {
        while (pos_ < text_.size() && iswspace(text_[pos_])) ++pos_;
    }

    bool Literal(wchar_t const* word) {
        size_t n = wcslen(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    std::optional<std::wstring> ReadString() {
        if (pos_ >= text_.size() || text_[pos_] != L'"') return std::nullopt;
        ++pos_;
        std::wstring out;
        while (pos_ < text_.size()) {
            wchar_t c = text_[pos_++];
            if (c == L'"') return out;
            if (c != L'\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) break;
            wchar_t esc = text_[pos_++];
            switch (esc) {
                case L'n': out.push_back(L'\n'); break;
                case L'r': out.push_back(L'\r'); break;
                case L't': out.push_back(L'\t'); break;
                case L'b': out.push_back(L'\b'); break;
                case L'f': out.push_back(L'\f'); break;
                case L'u': {
                    if (pos_ + 4 > text_.size()) return std::nullopt;
                    wchar_t code = 0;
                    for (int i = 0; i < 4; ++i) {
                        wchar_t d = text_[pos_++];
                        int v = (d >= L'0' && d <= L'9')   ? d - L'0'
                                : (d >= L'a' && d <= L'f') ? d - L'a' + 10
                                : (d >= L'A' && d <= L'F') ? d - L'A' + 10
                                                           : -1;
                        if (v < 0) return std::nullopt;
                        code = (wchar_t)(code * 16 + v);
                    }
                    out.push_back(code);
                    break;
                }
                default: out.push_back(esc); break;  // \" \\ \/ はそのまま
            }
        }
        return std::nullopt;
    }

    void Parse() {
        SkipSpace();
        if (pos_ >= text_.size() || text_[pos_] != L'{') return;
        ++pos_;
        for (;;) {
            SkipSpace();
            if (pos_ >= text_.size()) return;
            if (text_[pos_] == L'}') return;
            if (text_[pos_] == L',') {
                ++pos_;
                continue;
            }
            auto key = ReadString();
            if (!key) return;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != L':') return;
            ++pos_;
            SkipSpace();
            if (pos_ >= text_.size()) return;

            Entry entry;
            entry.key = *key;
            wchar_t c = text_[pos_];
            if (c == L'"') {
                auto value = ReadString();
                if (!value) return;
                entry.kind = Kind::String;
                entry.text = *value;
            } else if (c == L't' && Literal(L"true")) {
                entry.kind = Kind::Bool;
                entry.boolean = true;
            } else if (c == L'f' && Literal(L"false")) {
                entry.kind = Kind::Bool;
                entry.boolean = false;
            } else if (c == L'n' && Literal(L"null")) {
                entry.kind = Kind::Null;
            } else {
                wchar_t* end = nullptr;
                double value = wcstod(text_.c_str() + pos_, &end);
                if (end == text_.c_str() + pos_) return;
                pos_ = (size_t)(end - text_.c_str());
                entry.kind = Kind::Number;
                entry.number = RoundToInt(value);
            }
            entries_.push_back(std::move(entry));
        }
    }

    std::wstring const& text_;
    size_t pos_ = 0;
    std::vector<Entry> entries_;
};

/// JSON の文字列として書き出す。非 ASCII は \uXXXX に逃がす (Python の
/// json.dumps の既定に合わせておくと、手で開いたときの見た目が変わらない)。
std::string JsonString(std::wstring const& value) {
    std::string out = "\"";
    for (wchar_t c : value) {
        switch (c) {
            case L'"': out += "\\\""; break;
            case L'\\': out += "\\\\"; break;
            case L'\n': out += "\\n"; break;
            case L'\r': out += "\\r"; break;
            case L'\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c > 0x7e) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += "\"";
    return out;
}

// --------------------------------------------------------------- ファイル

std::optional<std::wstring> ReadTextFile(std::wstring const& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string bytes((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr);
    CloseHandle(file);
    if (!ok) return std::nullopt;
    bytes.resize(read);
    // 手で編集したときの BOM 付きファイルも読めるように
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    return Utf8ToWide(bytes);
}

bool WriteTextFile(std::wstring const& path, std::string const& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

/// v1 以前の保存先。%APPDATA% が無い環境では空。
std::wstring LegacyConfigPath() {
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        return {};
    }
    std::wstring path = std::wstring(base) + L"\\MuuTask\\config.json";
    CoTaskMemFree(base);
    return path;
}

/// v1 以前が %APPDATA%\MuuTask\ に残した設定を引き継いで、その跡を消す。
///
/// 設定を exe と同じフォルダーへ移した目的が「やめるときにフォルダーごと消せば
/// 痕跡が残らないこと」なので、旧フォルダーを放置すると目的を果たせない。
/// 新しい場所に設定がまだ無いときだけ中身を移し、旧ファイルを消してから
/// フォルダーも消す。RemoveDirectory は空でなければ失敗するので、他人のものは
/// 残る。触るのは自分で作った %APPDATA%\MuuTask\ だけ。
void TakeOverLegacyConfig() {
    std::wstring const old = LegacyConfigPath();
    if (old.empty() || GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    std::wstring const current = Config::Path();
    if (GetFileAttributesW(current.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CopyFileW(old.c_str(), current.c_str(), TRUE);
    }
    if (DeleteFileW(old.c_str())) {
        size_t slash = old.find_last_of(L'\\');
        if (slash != std::wstring::npos) RemoveDirectoryW(old.substr(0, slash).c_str());
    }
}

}  // namespace

std::wstring Config::Path() { return ExeDirectory() + L"\\config.json"; }

Config Config::Load() {
    TakeOverLegacyConfig();

    Config config;
    auto text = ReadTextFile(Path());
    if (!text) return config;

    FlatJson json(*text);
    // 知らないキーは黙って無視する (前後の版で行き来できるように)。
    if (auto v = json.String(L"session")) config.session = *v;
    if (auto v = json.String(L"bar_anchor")) config.bar_anchor = *v;
    if (auto v = json.Bool(L"bar_hide_when_idle")) config.bar_hide_when_idle = *v;
    if (auto v = json.Int(L"bar_width")) config.bar_width = *v;
    if (auto v = json.Int(L"bar_gap")) config.bar_gap = *v;
    if (auto v = json.String(L"bar_bg")) config.bar_bg = *v;
    if (auto v = json.Bool(L"bar_wheel_volume")) config.bar_wheel_volume = *v;
    if (auto v = json.Int(L"popup_height")) config.popup_height = *v;
    return config;
}

void Config::Save() const {
    // 手で開いて編集する人がいるので、Python 版と同じ並び・同じ字下げで書く。
    std::string out = "{\n";
    auto line = [&out](char const* key, std::string const& value, bool last) {
        out += "  \"";
        out += key;
        out += "\": ";
        out += value;
        out += last ? "\n" : ",\n";
    };
    auto optional_string = [](std::optional<std::wstring> const& v) {
        return v ? JsonString(*v) : std::string("null");
    };

    line("session", optional_string(session), false);
    line("bar_anchor", JsonString(bar_anchor), false);
    line("bar_hide_when_idle", bar_hide_when_idle ? "true" : "false", false);
    line("bar_width", std::to_string(bar_width), false);
    line("bar_gap", std::to_string(bar_gap), false);
    line("bar_bg", optional_string(bar_bg), false);
    line("bar_wheel_volume", bar_wheel_volume ? "true" : "false", false);
    line("popup_height", std::to_string(popup_height), true);
    out += "}";

    WriteTextFile(Path(), out);
}
