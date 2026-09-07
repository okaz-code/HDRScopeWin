// HDRScopeのアプリアイコンを描画して Resources\AppIcon.ico を作ります。画像素材にも
// 外部ツールにも依存しません。
//   tools\make-icon.cmd
//
// 図案はmacOS版の Tools/make-icon.swift と同じです。暗い角丸背景（連続曲率に近い超楕円）に、
// 加算合成したR/G/Bのにじみ、その上に白い測定用レティクル。中央のセルはアプリ内の測定枠と
// 同じ「黒の太線＋白の細線」で描いています。64px未満では細い線が消えるため、リングだけの
// 簡略版になります。
//
// macOS版との唯一の違いは余白です。macOSは全アプリ共通のグリッドに合わせて9%空けますが、
// Windowsにその慣習はなく、同じだけ空けるとタスクバーで隣より小さく見えます。
#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowsapp.lib")

using winrt::com_ptr;
using winrt::check_hresult;

namespace {

// Windows has no shared icon grid, so the art only needs enough margin to keep the
// superellipse corners off the edge.
constexpr double kInset = 0.04;

struct Colour {
    double r = 0, g = 0, b = 0, a = 0;
};

Colour Over(Colour src, Colour dst) {
    double a = src.a + dst.a * (1 - src.a);
    if (a <= 0) return {};
    return {(src.r * src.a + dst.r * dst.a * (1 - src.a)) / a,
            (src.g * src.a + dst.g * dst.a * (1 - src.a)) / a,
            (src.b * src.a + dst.b * dst.a * (1 - src.a)) / a,
            a};
}

double Mix(double a, double b, double t) { return a + (b - a) * t; }

// Apple's icon shape is a continuous-curvature rounded square rather than a rounded
// rect with circular corners. A superellipse is the closest thing that is one line.
bool InsideSuperellipse(double dx, double dy, double half, double exponent = 5) {
    return std::pow(std::abs(dx / half), exponent) + std::pow(std::abs(dy / half), exponent) <= 1.0;
}

double DistanceToSegment(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay;
    double wx = px - ax, wy = py - ay;
    double length = vx * vx + vy * vy;
    double t = length > 0 ? std::clamp((wx * vx + wy * vy) / length, 0.0, 1.0) : 0.0;
    double cx = ax + vx * t, cy = ay + vy * t;
    return std::hypot(px - cx, py - cy);
}

// One supersample of the whole figure, in sRGB with straight alpha.
Colour Sample(double x, double y, int pixels) {
    const double size = pixels;
    const double inset = size * kInset;
    const double side = size - inset * 2;
    const double half = side / 2;
    const double cx = size / 2, cy = size / 2;
    const double dx = x - cx, dy = y - cy;

    Colour out{};
    if (InsideSuperellipse(dx, dy, half)) {
        // Vertical gradient, light at the top.
        double t = std::clamp((y - inset) / side, 0.0, 1.0);
        out = {Mix(0.129, 0.020, t), Mix(0.180, 0.027, t), Mix(0.322, 0.047, t), 1.0};

        // Additive R/G/B blooms: reads as "colour measurement" even at 16 px. The angles
        // are the macOS build's, converted from its y-up space to this y-down one.
        struct Bloom { double r, g, b, degrees; };
        static const Bloom blooms[] = {
            {1.00, 0.16, 0.22, 90}, {0.22, 1.00, 0.38, 210}, {0.26, 0.48, 1.00, 330},
        };
        const double offset = side * 0.150, radius = side * 0.265;
        for (const Bloom& bloom : blooms) {
            double angle = bloom.degrees * 3.14159265358979323846 / 180.0;
            double sx = cx + std::cos(angle) * offset;
            double sy = cy - std::sin(angle) * offset;
            double distance = std::hypot(x - sx, y - sy);
            if (distance >= radius) continue;
            double weight = 0.95 * (1.0 - distance / radius);
            out.r = std::min(1.0, out.r + bloom.r * weight);
            out.g = std::min(1.0, out.g + bloom.g * weight);
            out.b = std::min(1.0, out.b + bloom.b * weight);
        }
    }

    // Small sizes lose thin geometry entirely, so they keep only the ring.
    const bool detailed = pixels >= 64;
    const double minimumStroke = size <= 32 ? 1.0 : 1.5;

    double ringWidth = std::max(minimumStroke, side * 0.036);
    double ringRadius = side * 0.30;
    if (std::abs(std::hypot(dx, dy) - ringRadius) <= ringWidth / 2)
        out = Over({1, 1, 1, 0.95}, out);

    if (detailed) {
        double tickWidth = std::max(minimumStroke, side * 0.030);
        double from = side * 0.215, to = side * 0.385;
        for (int degrees = 0; degrees < 360; degrees += 90) {
            double angle = degrees * 3.14159265358979323846 / 180.0;
            double ax = cx + std::cos(angle) * from, ay = cy - std::sin(angle) * from;
            double bx = cx + std::cos(angle) * to, by = cy - std::sin(angle) * to;
            if (DistanceToSegment(x, y, ax, ay, bx, by) <= tickWidth / 2) {
                out = Over({1, 1, 1, 0.95}, out);
                break;
            }
        }

        // The same black-under-white frame the app draws around a measured pixel.
        double cell = side * 0.130, cellHalf = cell / 2;
        double chebyshev = std::max(std::abs(dx), std::abs(dy));
        double blackWidth = side * 0.040, whiteWidth = side * 0.018;
        if (std::abs(chebyshev - cellHalf) <= blackWidth / 2) out = Over({0, 0, 0, 0.85}, out);
        if (std::abs(chebyshev - cellHalf) <= whiteWidth / 2) out = Over({1, 1, 1, 1.0}, out);
    }
    return out;
}

// Supersampled so every edge in the figure gets antialiasing without a path rasterizer.
std::vector<uint8_t> Render(int pixels) {
    const int kSamples = 4;
    std::vector<uint8_t> bgra((size_t)pixels * pixels * 4);
    for (int y = 0; y < pixels; ++y) {
        for (int x = 0; x < pixels; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    Colour c = Sample(x + (sx + 0.5) / kSamples, y + (sy + 0.5) / kSamples, pixels);
                    // Accumulate premultiplied, which is what averaging coverage means.
                    r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                }
            }
            double n = kSamples * kSamples;
            r /= n; g /= n; b /= n; a /= n;
            uint8_t* p = &bgra[((size_t)y * pixels + x) * 4];
            auto byteOf = [](double v) { return (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255); };
            // The ICO format wants premultiplied-looking BGRA only for the mask; the
            // colour channels are straight, so undo the premultiply.
            p[0] = byteOf(a > 0 ? b / a : 0);
            p[1] = byteOf(a > 0 ? g / a : 0);
            p[2] = byteOf(a > 0 ? r / a : 0);
            p[3] = byteOf(a);
        }
    }
    return bgra;
}

void AppendU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF)); v.push_back((uint8_t)(x >> 8));
}
void AppendU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF)); v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF)); v.push_back((uint8_t)(x >> 24));
}

// A DIB icon image: a header claiming twice the height, bottom-up BGRA, then the 1bpp
// AND mask that predates alpha and that the shell still expects to be present.
std::vector<uint8_t> EncodeDib(const std::vector<uint8_t>& bgra, int pixels) {
    std::vector<uint8_t> out;
    size_t maskStride = (((size_t)pixels + 31) / 32) * 4;
    AppendU32(out, 40);
    AppendU32(out, (uint32_t)pixels);
    AppendU32(out, (uint32_t)pixels * 2);
    AppendU16(out, 1);
    AppendU16(out, 32);
    AppendU32(out, 0);                                    // BI_RGB
    AppendU32(out, (uint32_t)((size_t)pixels * pixels * 4 + maskStride * pixels));
    AppendU32(out, 0); AppendU32(out, 0);                 // pixels per metre
    AppendU32(out, 0); AppendU32(out, 0);                 // palette
    for (int y = pixels - 1; y >= 0; --y)
        out.insert(out.end(), bgra.begin() + (size_t)y * pixels * 4,
                   bgra.begin() + ((size_t)y + 1) * pixels * 4);
    // Fully transparent pixels are masked out so that shells which ignore the alpha
    // channel still get the right silhouette.
    for (int y = pixels - 1; y >= 0; --y) {
        std::vector<uint8_t> row(maskStride, 0);
        for (int x = 0; x < pixels; ++x)
            if (bgra[((size_t)y * pixels + x) * 4 + 3] == 0) row[x / 8] |= (uint8_t)(0x80 >> (x % 8));
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

std::vector<uint8_t> EncodePng(const std::vector<uint8_t>& bgra, int pixels) {
    com_ptr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put())));
    com_ptr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    com_ptr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize((UINT)pixels, (UINT)pixels));
    GUID format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    check_hresult(frame->WritePixels((UINT)pixels, (UINT)pixels * 4, (UINT)bgra.size(),
                                     const_cast<BYTE*>(bgra.data())));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());

    HGLOBAL handle = nullptr;
    check_hresult(GetHGlobalFromStream(stream.get(), &handle));
    size_t size = GlobalSize(handle);
    const uint8_t* data = (const uint8_t*)GlobalLock(handle);
    std::vector<uint8_t> out(data, data + size);
    GlobalUnlock(handle);
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"Resources\\AppIcon.ico";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // The sizes the Windows shell asks for, plus 256 for the extra-large views. PNG is
    // used for the two biggest, which is what keeps the file from being a megabyte.
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<std::vector<uint8_t>> images;
    for (int pixels : sizes) {
        std::vector<uint8_t> bgra = Render(pixels);
        images.push_back(pixels >= 128 ? EncodePng(bgra, pixels) : EncodeDib(bgra, pixels));
    }

    std::vector<uint8_t> ico;
    AppendU16(ico, 0);                                    // reserved
    AppendU16(ico, 1);                                    // type: icon
    AppendU16(ico, (uint16_t)std::size(sizes));
    uint32_t offset = 6 + 16 * (uint32_t)std::size(sizes);
    for (size_t i = 0; i < std::size(sizes); ++i) {
        ico.push_back((uint8_t)(sizes[i] == 256 ? 0 : sizes[i]));   // 0 means 256
        ico.push_back((uint8_t)(sizes[i] == 256 ? 0 : sizes[i]));
        ico.push_back(0);                                 // palette entries
        ico.push_back(0);                                 // reserved
        AppendU16(ico, 1);                                // colour planes
        AppendU16(ico, 32);                               // bits per pixel
        AppendU32(ico, (uint32_t)images[i].size());
        AppendU32(ico, offset);
        offset += (uint32_t)images[i].size();
    }
    for (const std::vector<uint8_t>& image : images)
        ico.insert(ico.end(), image.begin(), image.end());

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"アイコンを書き出せません: %ls\n", path);
        return 1;
    }
    DWORD written = 0;
    WriteFile(file, ico.data(), (DWORD)ico.size(), &written, nullptr);
    CloseHandle(file);
    wprintf(L"%zu枚を格納したアイコンを生成: %ls (%zu バイト)\n", std::size(sizes), path, ico.size());
    return 0;
}
