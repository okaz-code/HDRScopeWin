// Verification harness, not part of the app. Captures a window by title through
// Windows.Graphics.Capture in scRGB float16 and writes two things: a tone mapped PNG so
// the rendering can be looked at, and the raw scRGB values at requested points.
//
// Pointing it at HDRScope showing its own test pattern closes the loop: a known linear
// value goes to the swap chain, the compositor hands it back, and the number that comes
// out says whether the display path scales correctly.
//
//   verify.exe "<window title substring>" out.png [x,y ...]
#include <windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

static HWND g_found = nullptr;
static std::wstring g_needle;

static BOOL CALLBACK Finder(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512];
    if (GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;
    if (std::wstring(title).find(g_needle) == std::wstring::npos) return TRUE;
    g_found = hwnd;
    return FALSE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: verify.exe \"<title substring>\" out.png [x,y ...]\n");
        return 2;
    }
    init_apartment(apartment_type::multi_threaded);
    g_needle = argv[1];
    EnumWindows(Finder, 0);
    if (!g_found) { printf("window not found: %ls\n", argv[1]); return 1; }

    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                      nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, ctx.put());
    auto dxgiDevice = device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
    auto rtDevice = inspectable.as<IDirect3DDevice>();

    auto interop = get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForWindow(g_found, guid_of<GraphicsCaptureItem>(), put_abi(item)));
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        rtDevice, DirectXPixelFormat::R16G16B16A16Float, 2, item.Size());
    auto session = pool.CreateCaptureSession(item);
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}
    session.StartCapture();
    Direct3D11CaptureFrame frame{nullptr};
    for (int i = 0; i < 300 && !frame; ++i) { frame = pool.TryGetNextFrame(); if (!frame) Sleep(10); }
    if (!frame) { printf("no frame\n"); return 1; }
    for (int i = 0; i < 25; ++i) {
        Sleep(15);
        if (auto newer = pool.TryGetNextFrame()) { frame.Close(); frame = newer; }
    }

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    access->GetInterface(guid_of<ID3D11Texture2D>(), texture.put_void());
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staged = desc;
    staged.Usage = D3D11_USAGE_STAGING;
    staged.BindFlags = 0;
    staged.MiscFlags = 0;
    staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    com_ptr<ID3D11Texture2D> staging;
    device->CreateTexture2D(&staged, nullptr, staging.put());
    ctx->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map);

    int width = (int)desc.Width, height = (int)desc.Height;
    printf("window %ls\ncaptured %d x %d\n", argv[1], width, height);

    std::vector<float> linear((size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        const uint16_t* row = (const uint16_t*)((const uint8_t*)map.pData + (size_t)y * map.RowPitch);
        for (int x = 0; x < width * 4; ++x)
            linear[(size_t)y * width * 4 + x] = DirectX::PackedVector::XMConvertHalfToFloat(row[x]);
    }
    ctx->Unmap(staging.get(), 0);

    // Whether a viewer is really rendering HDR shows up here: anything above the SDR
    // white level (scRGB = SDRWhiteLevel/80) is light the panel only makes in HDR mode.
    float peak = 0;
    size_t aboveSdr = 0;
    for (size_t i = 0; i < linear.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            peak = (std::max)(peak, linear[i + c]);
            if (linear[i + c] > 3.0f) ++aboveSdr;
        }
    printf("  peak scRGB %.4f (= %.0f nit)  above SDR white: %.3f%%\n",
           peak, peak * 80.0, 100.0 * aboveSdr / (linear.size() / 4 * 3));

    for (int i = 3; i < argc; ++i) {
        int x = 0, y = 0;
        if (swscanf_s(argv[i], L"%d,%d", &x, &y) != 2) continue;
        if (x < 0 || y < 0 || x >= width || y >= height) { printf("  (%d,%d) out of range\n", x, y); continue; }
        const float* p = &linear[((size_t)y * width + x) * 4];
        printf("  (%4d,%4d) scRGB R=%.5f G=%.5f B=%.5f A=%.5f\n", x, y, p[0], p[1], p[2], p[3]);
    }

    // Tone mapped preview so the rendering can be inspected as a normal image. This is
    // only for looking at; the numbers above are the measurement.
    std::vector<uint8_t> srgb((size_t)width * height * 4);
    for (size_t i = 0; i < srgb.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            double v = linear[i + c] / 3.0;               // undo a 240 nit SDR white
            v = v / (1.0 + v);                            // compress the HDR range
            v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            srgb[i + (2 - c)] = (uint8_t)std::lround(std::min(1.0, std::max(0.0, v)) * 255);
        }
        srgb[i + 3] = 255;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ptr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
    com_ptr<IWICStream> stream;
    wic->CreateStream(stream.put());
    stream->InitializeFromFilename(argv[2], GENERIC_WRITE);
    com_ptr<IWICBitmapEncoder> encoder;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    com_ptr<IWICBitmapFrameEncode> out;
    encoder->CreateNewFrame(out.put(), nullptr);
    out->Initialize(nullptr);
    out->SetSize(width, height);
    GUID format = GUID_WICPixelFormat32bppBGRA;
    out->SetPixelFormat(&format);
    out->WritePixels(height, width * 4, (UINT)srgb.size(), srgb.data());
    out->Commit();
    encoder->Commit();
    printf("preview written: %ls\n", argv[2]);
    return 0;
}
