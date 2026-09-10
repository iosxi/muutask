#pragma once

// 版番号の唯一の出どころ。v1 から配布のたびに 1 ずつ上げる。
// build.ps1 がこの行を読んで zip 名と README.txt の @VERSION@ に埋め込む。
#define MUUTASK_VERSION_NUM 22

// 数字をそのまま文字列にする (二重管理にしないため)。
#define MUUTASK_STRINGIFY2(x) #x
#define MUUTASK_STRINGIFY(x) MUUTASK_STRINGIFY2(x)
#define MUUTASK_VERSION MUUTASK_STRINGIFY(MUUTASK_VERSION_NUM)

#define MUUTASK_APP_NAME L"MuuTask"
