// exe とショートカット用の muutask.ico を作る。
//
// アプリ内で使っている音符 (artwork::NoteIcon) をそのまま流用するので、トレイの
// アイコンと見た目が揃う。アイコンはリソースとしてリンク前に要るため、ふだんの
// ビルドとは別の実行ファイルにしてある。音符の形や色を変えたときだけ動かす:
//
//   cmake --build build --target icontool
//   build\icontool.exe muutask.ico
#include <objbase.h>
#include <shlwapi.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

#include "artwork.h"
#include "common.h"
#include "image.h"

using Microsoft::WRL::ComPtr;

namespace {

// 下地と音符の色。Python 版の make_icon.py と同じ。
constexpr Rgb kBackground{0x1f, 0x6f, 0xeb};
constexpr Rgb kForeground{0xff, 0xff, 0xff};
constexpr int kBaseSize = 256;
constexpr int kSizes[] = {16, 24, 32, 48, 64, 128, 256};
// この大きさ以上は PNG で入れる。全部 BMP にすると ico だけで 290 KB になり、
// exe が膨らむ (実測: 全部 BMP だと ico だけで 290 KB)。
constexpr int kPngFrom = 48;

/// 乗算済みアルファを戻す。ico の中身は素のアルファ。
image::Bgra Unpremultiply(image::Bgra const& src) {
    image::Bgra out = src;
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        unsigned a = out.pixels[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            unsigned v = out.pixels[i + c] * 255u / a;
            out.pixels[i + c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    return out;
}

/// 32bpp の BMP として ico に入れる形 (ヘッダー + 下から上の画素 + AND マスク)。
std::vector<uint8_t> EncodeBmp(image::Bgra const& img) {
    int const w = img.width, h = img.height;
    size_t const mask_stride = (size_t)((w + 31) / 32) * 4;
    std::vector<uint8_t> out;

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = w;
    header.biHeight = h * 2;  // XOR と AND を積むので 2 倍で申告する決まり
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = (DWORD)((size_t)w * h * 4 + mask_stride * h);
    out.insert(out.end(), (uint8_t*)&header, (uint8_t*)&header + sizeof(header));

    for (int y = h - 1; y >= 0; --y) {  // 下から上へ
        uint8_t const* row = img.pixels.data() + (size_t)y * w * 4;
        out.insert(out.end(), row, row + (size_t)w * 4);
    }
    out.insert(out.end(), mask_stride * h, 0);  // アルファを使うのでマスクは全 0
    return out;
}

/// PNG として書き出す (大きい寸法用)。
std::vector<uint8_t> EncodePng(image::Bgra const& img) {
    std::vector<uint8_t> out;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return out;
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return out;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        return out;
    }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return out;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return out;
    if (FAILED(frame->Initialize(props.Get()))) return out;
    if (FAILED(frame->SetSize(img.width, img.height))) return out;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return out;
    if (FAILED(frame->WritePixels(img.height, img.width * 4,
                                  (UINT)img.pixels.size(),
                                  const_cast<BYTE*>(img.pixels.data())))) {
        return out;
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return out;

    HGLOBAL handle = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &handle))) return out;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return out;
    void* bytes = GlobalLock(handle);
    if (bytes) {
        out.assign((uint8_t*)bytes, (uint8_t*)bytes + (size_t)stat.cbSize.QuadPart);
        GlobalUnlock(handle);
    }
    return out;
}

#pragma pack(push, 1)
struct IconDir {
    uint16_t reserved = 0;
    uint16_t type = 1;  // 1 = アイコン
    uint16_t count = 0;
};
struct IconDirEntry {
    uint8_t width = 0;   // 256 のときは 0
    uint8_t height = 0;
    uint8_t colors = 0;
    uint8_t reserved = 0;
    uint16_t planes = 1;
    uint16_t bit_count = 32;
    uint32_t bytes = 0;
    uint32_t offset = 0;
};
#pragma pack(pop)

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: icontool <out.ico>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 256 で一度だけ描いてから、各寸法へ縮める。小さい寸法をそれぞれ描き直すと
    // 線の太さの見え方が揃わないため (Python 版も 256 から縮めていた)。
    image::Bgra const base = artwork::NoteIcon(kBaseSize, kForeground, kBackground,
                                               RoundToInt(kBaseSize * 0.22));

    std::vector<std::vector<uint8_t>> blobs;
    std::vector<IconDirEntry> entries;
    for (int size : kSizes) {
        image::Bgra icon;
        if (size == kBaseSize) {
            icon = base;
        } else {
            // 縮小は RGB 側と被覆率を別々に扱う必要がないので、
            // いったん素のアルファに戻してから畳む
            image::Bgra const straight = Unpremultiply(base);
            image::Rgb888 rgb;
            rgb.width = straight.width;
            rgb.height = straight.height;
            rgb.pixels.resize((size_t)rgb.width * rgb.height * 3);
            image::Mask alpha((size_t)rgb.width * rgb.height);
            for (size_t i = 0; i < alpha.size(); ++i) {
                rgb.pixels[i * 3 + 0] = straight.pixels[i * 4 + 2];
                rgb.pixels[i * 3 + 1] = straight.pixels[i * 4 + 1];
                rgb.pixels[i * 3 + 2] = straight.pixels[i * 4 + 0];
                alpha[i] = straight.pixels[i * 4 + 3];
            }
            image::Rgb888 small_rgb = image::Resize(rgb, size, size);
            // 被覆率も同じ倍率で畳む (角の丸みを保つため)
            image::Rgb888 alpha_as_rgb;
            alpha_as_rgb.width = rgb.width;
            alpha_as_rgb.height = rgb.height;
            alpha_as_rgb.pixels.resize(rgb.pixels.size());
            for (size_t i = 0; i < alpha.size(); ++i) {
                alpha_as_rgb.pixels[i * 3 + 0] = alpha[i];
                alpha_as_rgb.pixels[i * 3 + 1] = alpha[i];
                alpha_as_rgb.pixels[i * 3 + 2] = alpha[i];
            }
            image::Rgb888 small_alpha = image::Resize(alpha_as_rgb, size, size);

            icon.width = size;
            icon.height = size;
            icon.pixels.resize((size_t)size * size * 4);
            for (size_t i = 0; i < (size_t)size * size; ++i) {
                icon.pixels[i * 4 + 0] = small_rgb.pixels[i * 3 + 2];
                icon.pixels[i * 4 + 1] = small_rgb.pixels[i * 3 + 1];
                icon.pixels[i * 4 + 2] = small_rgb.pixels[i * 3 + 0];
                icon.pixels[i * 4 + 3] = small_alpha.pixels[i * 3];
            }
        }
        if (size == kBaseSize) icon = Unpremultiply(icon);

        blobs.push_back(size >= kPngFrom ? EncodePng(icon) : EncodeBmp(icon));
        if (blobs.back().empty()) {
            printf("%d px の書き出しに失敗しました\n", size);
            return 1;
        }
        IconDirEntry entry;
        entry.width = (uint8_t)(size >= 256 ? 0 : size);
        entry.height = entry.width;
        entry.bytes = (uint32_t)blobs.back().size();
        entries.push_back(entry);
    }

    IconDir dir;
    dir.count = (uint16_t)entries.size();
    uint32_t offset =
        (uint32_t)(sizeof(IconDir) + sizeof(IconDirEntry) * entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].offset = offset;
        offset += entries[i].bytes;
    }

    HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        printf("書き込めません\n");
        return 1;
    }
    DWORD written = 0;
    WriteFile(file, &dir, sizeof(dir), &written, nullptr);
    WriteFile(file, entries.data(), (DWORD)(sizeof(IconDirEntry) * entries.size()),
              &written, nullptr);
    for (auto const& blob : blobs) {
        WriteFile(file, blob.data(), (DWORD)blob.size(), &written, nullptr);
    }
    CloseHandle(file);

    printf("%ls を書き出しました (%u 寸法 / %u バイト)\n", argv[1], dir.count, offset);
    CoUninitialize();
    return 0;
}
