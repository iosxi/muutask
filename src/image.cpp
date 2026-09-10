#include "image.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

namespace image {
namespace {

constexpr int kSupersample = 4;

// 縁の帯を落とすときの、その一列の「平ら」さ (画素値のばらつきの上限)
constexpr double kPadFlatness = 3.0;
// 角の色とどれだけ近ければ帯と見なすか (0-255)
constexpr double kPadNearness = 6.0;
// 片側で落とせる割合の上限。絵そのものを食べないための歯止め
constexpr double kPadMaxTrim = 0.30;

IWICImagingFactory* Factory() {
    // 常駐アプリなので作り直さない。COM の初期化は呼び出し側で済ませてある。
    static ComPtr<IWICImagingFactory> factory = [] {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&f));
        return f;
    }();
    return factory.Get();
}

/// メモリ上のバイト列からフレームを 1 枚取り出す。
bool OpenFrame(std::vector<uint8_t> const& data, ComPtr<IWICBitmapFrameDecode>* frame) {
    IWICImagingFactory* factory = Factory();
    if (!factory || data.empty()) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()),
                                            (DWORD)data.size()))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand,
                                                &decoder))) {
        return false;
    }
    return SUCCEEDED(decoder->GetFrame(0, frame->ReleaseAndGetAddressOf()));
}

// ------------------------------------------------------------------ 面の道具

/// 1 面 (1 チャンネル) の値の並び。ぼかしはこの単位で行う。
struct Plane {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> v;

    uint8_t& at(int x, int y) { return v[(size_t)y * width + x]; }
    uint8_t at(int x, int y) const { return v[(size_t)y * width + x]; }
};

/// σ からガウスの重みを作る。半径は 3σ で打ち切る。
std::vector<double> GaussianKernel(double sigma, int* radius_out) {
    int radius = (int)std::ceil(sigma * 3.0);
    if (radius < 1) radius = 1;
    std::vector<double> weights((size_t)radius * 2 + 1);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        double w = std::exp(-(double)i * i / (2.0 * sigma * sigma));
        weights[(size_t)(i + radius)] = w;
        sum += w;
    }
    for (double& w : weights) w /= sum;
    *radius_out = radius;
    return weights;
}

/// 分離型のガウスぼかし。Pillow は箱ぼかし 3 回で近似しているが、こちらは
/// 素直にガウスを畳む (σ が 0.5〜3 の範囲なので、見た目の差は出ない)。
void GaussianBlur(Plane& plane, double sigma) {
    if (sigma <= 0.0 || plane.width <= 0 || plane.height <= 0) return;
    int radius = 0;
    std::vector<double> const weights = GaussianKernel(sigma, &radius);

    std::vector<uint8_t> tmp(plane.v.size());
    // 横
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            double sum = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                int sx = Clamp(x + i, 0, plane.width - 1);  // 端は伸ばす
                sum += weights[(size_t)(i + radius)] * plane.at(sx, y);
            }
            tmp[(size_t)y * plane.width + x] =
                (uint8_t)Clamp(RoundToInt(sum), 0, 255);
        }
    }
    // 縦
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            double sum = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                int sy = Clamp(y + i, 0, plane.height - 1);
                sum += weights[(size_t)(i + radius)] * tmp[(size_t)sy * plane.width + x];
            }
            plane.at(x, y) = (uint8_t)Clamp(RoundToInt(sum), 0, 255);
        }
    }
}

// ------------------------------------------------------------------ YCbCr

/// JPEG と同じ full-range の YCbCr。Pillow の convert("YCbCr") と同じ式。
void ToYCbCr(Rgb888 const& src, Plane* y, Plane* cb, Plane* cr) {
    y->width = cb->width = cr->width = src.width;
    y->height = cb->height = cr->height = src.height;
    size_t const n = (size_t)src.width * src.height;
    y->v.resize(n);
    cb->v.resize(n);
    cr->v.resize(n);
    for (size_t i = 0; i < n; ++i) {
        double r = src.pixels[i * 3 + 0];
        double g = src.pixels[i * 3 + 1];
        double b = src.pixels[i * 3 + 2];
        y->v[i] = (uint8_t)Clamp(RoundToInt(0.299 * r + 0.587 * g + 0.114 * b), 0, 255);
        cb->v[i] = (uint8_t)Clamp(
            RoundToInt(-0.168736 * r - 0.331264 * g + 0.5 * b + 128.0), 0, 255);
        cr->v[i] = (uint8_t)Clamp(
            RoundToInt(0.5 * r - 0.418688 * g - 0.081312 * b + 128.0), 0, 255);
    }
}

void FromYCbCr(Plane const& y, Plane const& cb, Plane const& cr, Rgb888* dst) {
    size_t const n = (size_t)y.width * y.height;
    dst->width = y.width;
    dst->height = y.height;
    dst->pixels.resize(n * 3);
    for (size_t i = 0; i < n; ++i) {
        double luma = y.v[i];
        double u = (double)cb.v[i] - 128.0;
        double v = (double)cr.v[i] - 128.0;
        dst->pixels[i * 3 + 0] = (uint8_t)Clamp(RoundToInt(luma + 1.402 * v), 0, 255);
        dst->pixels[i * 3 + 1] =
            (uint8_t)Clamp(RoundToInt(luma - 0.344136 * u - 0.714136 * v), 0, 255);
        dst->pixels[i * 3 + 2] = (uint8_t)Clamp(RoundToInt(luma + 1.772 * u), 0, 255);
    }
}

// ------------------------------------------------------------------ Lanczos

double Sinc(double x) {
    if (x == 0.0) return 1.0;
    x *= 3.14159265358979323846;
    return std::sin(x) / x;
}

double Lanczos3(double x) {
    if (x < 0.0) x = -x;
    if (x >= 3.0) return 0.0;
    return Sinc(x) * Sinc(x / 3.0);
}

/// 1 方向ぶんの重み表。Pillow の precompute_coeffs と同じ組み立て。
struct Coeffs {
    int support_span = 0;              // 1 出力あたりの参照数の上限
    std::vector<int> starts;           // 参照の開始位置
    std::vector<double> weights;       // starts.size() * support_span
};

Coeffs BuildCoeffs(int in_size, int out_size) {
    Coeffs c;
    double const scale = (double)in_size / out_size;
    // 縮小するときはフィルターを伸ばす (伸ばさないと折り返しが出る)
    double const filter_scale = scale < 1.0 ? 1.0 : scale;
    double const support = 3.0 * filter_scale;
    c.support_span = (int)std::ceil(support) * 2 + 1;
    c.starts.resize((size_t)out_size);
    c.weights.assign((size_t)out_size * c.support_span, 0.0);

    for (int xx = 0; xx < out_size; ++xx) {
        double const center = (xx + 0.5) * scale;
        double const ss = 1.0 / filter_scale;
        int xmin = (int)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        int const count = xmax - xmin;
        c.starts[(size_t)xx] = xmin;

        double sum = 0.0;
        double* w = c.weights.data() + (size_t)xx * c.support_span;
        for (int i = 0; i < count && i < c.support_span; ++i) {
            double weight = Lanczos3((i + xmin - center + 0.5) * ss);
            w[i] = weight;
            sum += weight;
        }
        if (sum != 0.0) {
            for (int i = 0; i < c.support_span; ++i) w[i] /= sum;
        } else if (count > 0) {
            w[0] = 1.0;  // 念のため。ここに来ることは無いはず
        }
    }
    return c;
}

}  // namespace

// --------------------------------------------------------------- 読み込み

std::optional<Rgb888> Decode(std::vector<uint8_t> const& data) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!OpenFrame(data, &frame)) return std::nullopt;

    ComPtr<IWICBitmapSource> source;
    // 24bppBGR に揃える。RGBA が来ても、Pillow の convert("RGB") と同じく
    // アルファは捨てるだけにする (アルバム アートは実質すべて不透明)。
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat24bppBGR, frame.Get(),
                                      &source))) {
        return std::nullopt;
    }
    UINT width = 0, height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0) {
        return std::nullopt;
    }
    // 桁外れの絵は扱わない (壊れたデータで山ほど確保しないため)
    if (width > 8192 || height > 8192) return std::nullopt;

    UINT const stride = width * 3;
    std::vector<uint8_t> raw((size_t)stride * height);
    if (FAILED(source->CopyPixels(nullptr, stride, (UINT)raw.size(), raw.data()))) {
        return std::nullopt;
    }

    Rgb888 out;
    out.width = (int)width;
    out.height = (int)height;
    out.pixels.resize(raw.size());
    for (size_t i = 0; i < raw.size(); i += 3) {  // BGR -> RGB
        out.pixels[i + 0] = raw[i + 2];
        out.pixels[i + 1] = raw[i + 1];
        out.pixels[i + 2] = raw[i + 0];
    }
    return out;
}

std::optional<SIZE> DecodeSize(std::vector<uint8_t> const& data) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!OpenFrame(data, &frame)) return std::nullopt;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height))) return std::nullopt;
    return SIZE{(LONG)width, (LONG)height};
}

// --------------------------------------------------------------- 加工

Rgb888 Crop(Rgb888 const& src, int left, int top, int width, int height) {
    Rgb888 out;
    if (width <= 0 || height <= 0) return out;
    out.width = width;
    out.height = height;
    out.pixels.resize((size_t)width * height * 3);
    for (int y = 0; y < height; ++y) {
        memcpy(out.pixels.data() + (size_t)y * width * 3, src.at(left, top + y),
               (size_t)width * 3);
    }
    return out;
}

Rgb888 CenterSquare(Rgb888 const& src) {
    int const side = src.width < src.height ? src.width : src.height;
    return Crop(src, (src.width - side) / 2, (src.height - side) / 2, side, side);
}

Rgb888 TrimPadding(Rgb888 const& src) {
    if (src.width < 3 || src.height < 3) return src;
    uint8_t const* corner = src.at(0, 0);

    // 1 列 (または 1 行) が「平らで、角と同じ色」かどうか。
    auto is_pad = [&](int x0, int y0, int x1, int y1) {
        double sum[3] = {0, 0, 0};
        double sq[3] = {0, 0, 0};
        int count = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                uint8_t const* p = src.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    sum[c] += p[c];
                    sq[c] += (double)p[c] * p[c];
                }
                ++count;
            }
        }
        if (count == 0) return false;
        for (int c = 0; c < 3; ++c) {
            double mean = sum[c] / count;
            double var = sq[c] / count - mean * mean;
            if (var < 0) var = 0;
            if (std::sqrt(var) > kPadFlatness) return false;
            if (std::fabs(mean - corner[c]) > kPadNearness) return false;
        }
        return true;
    };

    int top = 0, bottom = 0, left = 0, right = 0;
    while (top < src.height * kPadMaxTrim && is_pad(0, top, src.width, top + 1)) ++top;
    while (bottom < src.height * kPadMaxTrim &&
           is_pad(0, src.height - 1 - bottom, src.width, src.height - bottom)) {
        ++bottom;
    }
    while (left < src.width * kPadMaxTrim && is_pad(left, 0, left + 1, src.height)) ++left;
    while (right < src.width * kPadMaxTrim &&
           is_pad(src.width - 1 - right, 0, src.width - right, src.height)) {
        ++right;
    }
    if (!top && !bottom && !left && !right) return src;
    int const width = src.width - left - right;
    int const height = src.height - top - bottom;
    if (width <= 0 || height <= 0) return src;
    return Crop(src, left, top, width, height);
}

Rgb888 Resize(Rgb888 const& src, int width, int height) {
    Rgb888 out;
    if (src.empty() || width <= 0 || height <= 0) return out;
    if (width == src.width && height == src.height) return src;

    // 横方向 -> 縦方向の 2 段。途中は丸め誤差を溜めないよう double で持つ。
    Coeffs const hx = BuildCoeffs(src.width, width);
    std::vector<double> mid((size_t)width * src.height * 3);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < width; ++x) {
            double const* w = hx.weights.data() + (size_t)x * hx.support_span;
            int const start = hx.starts[(size_t)x];
            double acc[3] = {0, 0, 0};
            for (int i = 0; i < hx.support_span; ++i) {
                int sx = start + i;
                if (sx >= src.width) break;
                uint8_t const* p = src.at(sx, y);
                acc[0] += w[i] * p[0];
                acc[1] += w[i] * p[1];
                acc[2] += w[i] * p[2];
            }
            double* dst = mid.data() + ((size_t)y * width + x) * 3;
            dst[0] = acc[0];
            dst[1] = acc[1];
            dst[2] = acc[2];
        }
    }

    Coeffs const vy = BuildCoeffs(src.height, height);
    out.width = width;
    out.height = height;
    out.pixels.resize((size_t)width * height * 3);
    for (int y = 0; y < height; ++y) {
        double const* w = vy.weights.data() + (size_t)y * vy.support_span;
        int const start = vy.starts[(size_t)y];
        for (int x = 0; x < width; ++x) {
            double acc[3] = {0, 0, 0};
            for (int i = 0; i < vy.support_span; ++i) {
                int sy = start + i;
                if (sy >= src.height) break;
                double const* p = mid.data() + ((size_t)sy * width + x) * 3;
                acc[0] += w[i] * p[0];
                acc[1] += w[i] * p[1];
                acc[2] += w[i] * p[2];
            }
            uint8_t* dst = out.at(x, y);
            for (int c = 0; c < 3; ++c) {
                dst[c] = (uint8_t)Clamp(RoundToInt(acc[c]), 0, 255);
            }
        }
    }
    return out;
}

void BlurChroma(Rgb888& img, double sigma) {
    if (img.empty()) return;
    Plane y, cb, cr;
    ToYCbCr(img, &y, &cb, &cr);
    GaussianBlur(cb, sigma);
    GaussianBlur(cr, sigma);
    FromYCbCr(y, cb, cr, &img);
}

void SharpenLuma(Rgb888& img, double radius, int percent, int threshold) {
    if (img.empty()) return;
    Plane y, cb, cr;
    ToYCbCr(img, &y, &cb, &cr);

    Plane blurred = y;
    GaussianBlur(blurred, radius);
    // Pillow の UnsharpMask と同じ: 差が閾値を「超えた」ところだけ持ち上げる。
    // 閾値を置くと平らな面が荒れない。
    for (size_t i = 0; i < y.v.size(); ++i) {
        int diff = (int)y.v[i] - (int)blurred.v[i];
        if (diff > threshold || diff < -threshold) {
            y.v[i] = (uint8_t)Clamp(RoundToInt(y.v[i] + diff * percent / 100.0), 0, 255);
        }
    }
    FromYCbCr(y, cb, cr, &img);
}

double MeanAbsDiff(Rgb888 const& a, Rgb888 const& b) {
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) return 255.0;
    double sum = 0.0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        sum += std::abs((int)a.pixels[i] - (int)b.pixels[i]);
    }
    return sum / a.pixels.size();
}

// --------------------------------------------------------------- 形

Mask RasterizeSupersampled(int width, int height,
                           std::function<void(HDC, int)> const& draw) {
    Mask mask((size_t)width * height, 0);
    if (width <= 0 || height <= 0) return mask;

    int const ss = kSupersample;
    int const big_w = width * ss;
    int const big_h = height * ss;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = big_w;
    info.bmiHeader.biHeight = -big_h;  // 上から下へ
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return mask;
    HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(dc);
        return mask;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, dib);
    memset(bits, 0, (size_t)big_w * big_h * 4);

    // 白で塗ってもらう。輪郭のなめらかさは 4x4 の平均で作る。
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ old_brush = SelectObject(dc, white);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    draw(dc, ss);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(white);

    GdiFlush();
    auto const* pixels = (uint8_t const*)bits;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned sum = 0;
            for (int dy = 0; dy < ss; ++dy) {
                uint8_t const* row = pixels + ((size_t)(y * ss + dy) * big_w) * 4;
                for (int dx = 0; dx < ss; ++dx) sum += row[(size_t)(x * ss + dx) * 4];
            }
            mask[(size_t)y * width + x] = (uint8_t)(sum / (ss * ss));
        }
    }

    SelectObject(dc, old_bitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    return mask;
}

Mask RoundedRectMask(int width, int height, int radius) {
    return RasterizeSupersampled(width, height, [width, height, radius](HDC dc, int ss) {
        // RoundRect の右下は含まれないので +1 して端まで塗る
        RoundRect(dc, 0, 0, width * ss + 1, height * ss + 1, radius * ss * 2,
                  radius * ss * 2);
    });
}

// --------------------------------------------------------------- 出力

Bgra Premultiply(Rgb888 const& rgb, Mask const& alpha) {
    Bgra out;
    out.width = rgb.width;
    out.height = rgb.height;
    size_t const n = (size_t)rgb.width * rgb.height;
    out.pixels.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        unsigned a = i < alpha.size() ? alpha[i] : 255;
        out.pixels[i * 4 + 0] = (uint8_t)(rgb.pixels[i * 3 + 2] * a / 255);
        out.pixels[i * 4 + 1] = (uint8_t)(rgb.pixels[i * 3 + 1] * a / 255);
        out.pixels[i * 4 + 2] = (uint8_t)(rgb.pixels[i * 3 + 0] * a / 255);
        out.pixels[i * 4 + 3] = (uint8_t)a;
    }
    return out;
}

Bgra Premultiply(Rgb color, Mask const& alpha, int width, int height) {
    Bgra out;
    out.width = width;
    out.height = height;
    size_t const n = (size_t)width * height;
    out.pixels.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        unsigned a = i < alpha.size() ? alpha[i] : 0;
        out.pixels[i * 4 + 0] = (uint8_t)(color.b * a / 255);
        out.pixels[i * 4 + 1] = (uint8_t)(color.g * a / 255);
        out.pixels[i * 4 + 2] = (uint8_t)(color.r * a / 255);
        out.pixels[i * 4 + 3] = (uint8_t)a;
    }
    return out;
}

void CompositeOver(Bgra& dst, Bgra const& src) {
    if (dst.width != src.width || dst.height != src.height) return;
    for (size_t i = 0; i < dst.pixels.size(); i += 4) {
        unsigned sa = src.pixels[i + 3];
        unsigned inv = 255 - sa;
        for (int c = 0; c < 4; ++c) {
            dst.pixels[i + c] =
                (uint8_t)(src.pixels[i + c] + (dst.pixels[i + c] * inv + 127) / 255);
        }
    }
}

void Dim(Bgra& img, double amount) {
    double const keep = 1.0 - amount;
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            img.pixels[i + c] = (uint8_t)Clamp(RoundToInt(img.pixels[i + c] * keep), 0, 255);
        }
    }
}

HBITMAP CreateDib(Bgra const& img) {
    if (img.empty()) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = img.width;
    info.bmiHeader.biHeight = -img.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (dib && bits) memcpy(bits, img.pixels.data(), img.pixels.size());
    return dib;
}

void AlphaBlit(HDC dc, int x, int y, Bgra const& img) {
    if (img.empty()) return;
    HBITMAP dib = CreateDib(img);
    if (!dib) return;
    HDC src = CreateCompatibleDC(dc);
    if (src) {
        HGDIOBJ old = SelectObject(src, dib);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, x, y, img.width, img.height, src, 0, 0, img.width, img.height,
                   blend);
        SelectObject(src, old);
        DeleteDC(src);
    }
    DeleteObject(dib);
}

}  // namespace image
