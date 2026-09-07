#include "Capture.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <DirectXPackedVector.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <cmath>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::com_ptr;
using winrt::check_hresult;

std::wstring WindowItem::SizeText() const {
    return std::to_wstring(width) + L" × " + std::to_wstring(height) + L" px";
}

namespace {

bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// Windows.Graphics.Capture hands back the window at its extended frame bounds: the
// visible window, without the invisible resize border and without the drop shadow. Using
// the same rectangle for the listed size keeps the list honest about what a capture of
// that window will contain.
RECT VisibleBounds(HWND hwnd) {
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
        GetWindowRect(hwnd, &bounds);
    return bounds;
}

std::wstring ProcessName(DWORD pid, uint64_t& startTime) {
    startTime = 0;
    winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process) return L"不明なアプリ";
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
        startTime = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    wchar_t path[MAX_PATH * 2];
    DWORD size = (DWORD)std::size(path);
    if (!QueryFullProcessImageNameW(process.get(), 0, path, &size)) return L"不明なアプリ";
    std::wstring full(path, size);
    size_t slash = full.find_last_of(L'\\');
    std::wstring name = slash == std::wstring::npos ? full : full.substr(slash + 1);
    if (name.size() > 4) {
        std::wstring tail = name.substr(name.size() - 4);
        for (wchar_t& c : tail) c = (wchar_t)towlower(c);
        if (tail == L".exe") name.resize(name.size() - 4);
    }
    return name;
}

bool Describe(HWND hwnd, WindowItem& item) {
    if (!IsWindow(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return false;

    RECT bounds = VisibleBounds(hwnd);
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    int titleLength = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (titleLength > 0) {
        title.resize((size_t)titleLength);
        title.resize((size_t)GetWindowTextW(hwnd, title.data(), titleLength + 1));
    }

    item.hwnd = hwnd;
    item.pid = pid;
    item.app = ProcessName(pid, item.processStart);
    item.hasTitle = !title.empty();
    item.title = item.hasTitle ? title : L"タイトルなし";
    item.onScreen = IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsCloaked(hwnd);
    item.toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
    item.width = width;
    item.height = height;
    return true;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* list = reinterpret_cast<std::vector<WindowItem>*>(param);
    WindowItem item;
    if (Describe(hwnd, item)) list->push_back(std::move(item));
    return TRUE;
}

float HalfToFloat(uint16_t h) { return DirectX::PackedVector::XMConvertHalfToFloat(h); }

}  // namespace

std::vector<WindowItem> EnumerateWindows() {
    std::vector<WindowItem> list;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&list));
    std::sort(list.begin(), list.end(), [](const WindowItem& a, const WindowItem& b) {
        if (a.app != b.app) return a.app < b.app;
        if (a.title != b.title) return a.title < b.title;
        return a.Id() < b.Id();
    });
    return list;
}

bool RefreshWindow(WindowItem& item) {
    WindowItem fresh;
    if (!Describe(item.hwnd, fresh)) return false;
    // A handle can be reused by a different window, and a PID by a different process.
    // Both have to still be the ones that were captured before, or this is a new target.
    if (fresh.pid != item.pid || fresh.processStart != item.processStart) return false;
    fresh.processStart = item.processStart;
    item = fresh;
    return true;
}

bool CaptureSupported() {
    try {
        return GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

CaptureOutcome CaptureWindow(const WindowItem& item) {
    if (!CaptureSupported())
        throw ScopeError("このWindowsではウィンドウキャプチャ（Windows.Graphics.Capture）を利用できません。");

    CaptureOutcome outcome;
    outcome.display = QueryDisplayForWindow(item.hwnd);
    outcome.normalization = outcome.display.ScrgbPerSdrWhite();

    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    if (FAILED(hr))
        throw ScopeError("Direct3D 11デバイスを作成できません：" + HresultMessage((long)hr));

    auto dxgiDevice = device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    auto captureDevice = inspectable.as<IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem captureItem{nullptr};
    hr = interop->CreateForWindow(item.hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(captureItem));
    if (FAILED(hr) || !captureItem)
        throw ScopeError("対象ウィンドウをキャプチャ対象にできません。閉じられた可能性があります：" + HresultMessage((long)hr));

    auto size = captureItem.Size();
    if (size.Width <= 0 || size.Height <= 0)
        throw ScopeError("対象ウィンドウの大きさが0です。");

    // Float16 is what makes this an HDR capture: the composed window arrives in scRGB
    // with values above SDR white intact, instead of an 8 bit surface clipped to SDR.
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        captureDevice, DirectXPixelFormat::R16G16B16A16Float, 2, size);
    auto session = pool.CreateCaptureSession(captureItem);
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}
    session.StartCapture();

    Direct3D11CaptureFrame frame{nullptr};
    for (int i = 0; i < 300 && !frame; ++i) {
        frame = pool.TryGetNextFrame();
        if (!frame) Sleep(10);
    }
    if (!frame) {
        session.Close();
        pool.Close();
        throw ScopeError("キャプチャフレームを取得できませんでした。ウィンドウが最小化されていないか確認してください。");
    }
    // The first frame can predate the window's next paint, so keep taking newer ones for
    // a moment and measure the latest. A window that never repaints simply keeps the first.
    for (int i = 0; i < 20; ++i) {
        Sleep(15);
        if (auto newer = pool.TryGetNextFrame()) {
            frame.Close();
            frame = newer;
        }
    }

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        frame.Close(); session.Close(); pool.Close();
        throw ScopeError("OSが浮動小数点HDRサーフェスを返しませんでした。SDR画像へのフォールバックは行いません。");
    }

    D3D11_TEXTURE2D_DESC staged = desc;
    staged.Usage = D3D11_USAGE_STAGING;
    staged.BindFlags = 0;
    staged.MiscFlags = 0;
    staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    com_ptr<ID3D11Texture2D> staging;
    check_hresult(device->CreateTexture2D(&staged, nullptr, staging.put()));
    context->CopyResource(staging.get(), texture.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));

    int width = (int)desc.Width, height = (int)desc.Height;
    std::vector<float> pixels((size_t)width * height * 4);
    double scale = outcome.normalization > 0 ? 1.0 / outcome.normalization : 1.0;
    for (int y = 0; y < height; ++y) {
        const uint16_t* row = (const uint16_t*)((const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch);
        float* out = pixels.data() + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x) {
            float r = HalfToFloat(row[x * 4 + 0]);
            float g = HalfToFloat(row[x * 4 + 1]);
            float b = HalfToFloat(row[x * 4 + 2]);
            float a = HalfToFloat(row[x * 4 + 3]);
            // The compositor works in premultiplied alpha. Store and measure straight
            // RGB, the same convention the file formats and the macOS build use.
            if (a > 0 && a != 1) { r /= a; g /= a; b /= a; }
            out[x * 4 + 0] = (float)(r * scale);
            out[x * 4 + 1] = (float)(g * scale);
            out[x * 4 + 2] = (float)(b * scale);
            out[x * 4 + 3] = a;
        }
    }
    context->Unmap(staging.get(), 0);
    frame.Close();
    session.Close();
    pool.Close();

    for (float v : pixels)
        if (!std::isfinite(v)) throw ScopeError("キャプチャに非有限の画素値が含まれています。");

    outcome.image = std::make_shared<PixelImage>(width, height, std::move(pixels));
    return outcome;
}
