#include "errlog.h"

#include <windows.h>

#include <mutex>

#include "common.h"

namespace errlog {
namespace {

constexpr uint64_t kMaxBytes = 256 * 1024;

std::mutex g_lock;

std::wstring Stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buf;
}

}  // namespace

std::wstring LogPath() { return ExeDirectory() + L"\\MuuTask.log"; }

void Write(std::wstring const& message) {
    std::lock_guard<std::mutex> guard(g_lock);
    std::wstring const path = LogPath();

    // 上限を超えていたら捨てて書き直す。常駐しっぱなしなので、放っておくと
    // 際限なく育つ。
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        uint64_t size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        if (size > kMaxBytes) DeleteFileW(path.c_str());
    }

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    // 書き込めない場所 (Program Files 直下など) では黙って諦める。その回の
    // 動作は続けられる。
    if (file == INVALID_HANDLE_VALUE) return;

    std::string line = WideToUtf8(L"[" + Stamp() + L"] " + message + L"\r\n");
    DWORD written = 0;
    WriteFile(file, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(file);
}

void Exception(std::wstring const& message, std::wstring const& detail) {
    Write(detail.empty() ? message : message + L"\n" + detail);
}

}  // namespace errlog
