cmake_minimum_required(VERSION 3.21)
project(MuuTask CXX)

# C++/WinRT が C++20 のコルーチン・機能検査マクロを前提にしている
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 配布物を exe 1 つに戻すため、CRT は静的リンクにする (VC 再頒布可能パッケージを
# 要求しない)。Python 版の lib\ が要らなくなるのはこれのおかげ。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# rc.exe は既定でソースをシステムのコード ページ (日本語環境なら 932) として
# 読む。UTF-8 の日本語コメントが 2 バイト文字として対に組まれ、行末の改行まで
# 巻き込んで**次の #define 行ごと食われる**ため、明示的に UTF-8 を指定する
# (実測。version.h の MUUTASK_VERSION_NUM が未定義になった)。
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /c65001")

add_executable(MuuTask WIN32
    src/main.cpp
    src/app.cpp
    src/common.cpp
    src/errlog.cpp
    src/config.cpp
    src/theme.cpp
    src/win32util.cpp
    src/image.cpp
    src/artwork.cpp
    src/media.cpp
    src/menu.cpp
    src/bar.cpp
    src/popup.cpp
    src/tray.cpp
    src/muutask.rc
)

# rc.exe からも version.h を読めるようにする
target_include_directories(MuuTask PRIVATE src)

target_compile_definitions(MuuTask PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ソースは UTF-8。付けないと日本語コメントが CP932 として読まれて warning C4819 が出る
target_compile_options(MuuTask PRIVATE /utf-8 /W4 /permissive- /EHsc)

target_link_libraries(MuuTask PRIVATE
    user32 gdi32 msimg32 shell32 shlwapi advapi32 ole32 windowscodecs
    windowsapp   # C++/WinRT (RoGetActivationFactory ほか)
)

# 常駐アプリなので、大きさと起動の速さを優先する
target_link_options(MuuTask PRIVATE
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

# muutask.ico を書き出す道具。アイコンはリソースとしてリンク前に要るので、
# ふだんのビルドには含めない (EXCLUDE_FROM_ALL)。音符の形や色を変えたときだけ:
#   cmake --build build --target icontool
#   build\icontool.exe muutask.ico
add_executable(icontool EXCLUDE_FROM_ALL
    src/icontool.cpp
    src/artwork.cpp
    src/image.cpp
    src/common.cpp
)
target_compile_definitions(icontool PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(icontool PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_include_directories(icontool PRIVATE src)
target_link_libraries(icontool PRIVATE user32 gdi32 msimg32 shlwapi ole32 windowscodecs)
