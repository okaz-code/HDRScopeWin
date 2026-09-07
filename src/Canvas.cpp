#include "Canvas.h"
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>

using winrt::com_ptr;

namespace {

const wchar_t kCanvasClass[] = L"HDRScopeCanvas";

// The display path mirrors the picker: nearest pixels when magnified so individual
// pixels stay distinguishable, and the same area-weighted mean when shrunk so what is on
// screen is the same average the readout reports. The one departure is the sample stride:
// at extreme zoom-out an exact per-pixel loop would run tens of thousands of iterations
// per output pixel, so beyond a 32x32 box the cells are sampled on a stride. The picker
// itself always integrates exactly, on the CPU, so no measured number depends on this.
const char kShaderSource[] = R"(
cbuffer Params : register(b0) {
    float2 origin;
    float  zoom;
    float  displayScale;
    float2 imageSize;
    float2 sampleMin;
    float2 sampleMax;
    float  hasSample;
    float  encodeSrgb;
};

Texture2D<float4> source : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 p[3] = { float2(-1, 1), float2(3, 1), float2(-1, -3) };
    return float4(p[id], 0, 1);
}

float3 LinearToSrgb(float3 v) {
    float3 s = sign(v);
    float3 a = abs(v);
    return s * (a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055);
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    float3 background = float3(0.018, 0.018, 0.022);
    float2 xy = (position.xy - origin) / zoom;
    float3 rgb;
    if (any(xy < 0) || any(xy >= imageSize)) {
        rgb = background;
    } else {
        float4 c;
        if (zoom >= 1.0) {
            c = source.Load(int3((int2)xy, 0));
        } else {
            float2 lo = max(float2(0, 0), (position.xy - 0.5 - origin) / zoom);
            float2 hi = min(imageSize, (position.xy + 0.5 - origin) / zoom);
            int x0 = (int)floor(lo.x), x1 = (int)ceil(hi.x);
            int y0 = (int)floor(lo.y), y1 = (int)ceil(hi.y);
            int stepX = max(1, (x1 - x0 + 31) / 32);
            int stepY = max(1, (y1 - y0 + 31) / 32);
            float4 sum = float4(0, 0, 0, 0);
            float area = 0;
            for (int y = y0; y < y1; y += stepY) {
                float dy = max(0.0, min((float)(y + stepY), hi.y) - max((float)y, lo.y));
                for (int x = x0; x < x1; x += stepX) {
                    float dx = max(0.0, min((float)(x + stepX), hi.x) - max((float)x, lo.x));
                    float w = dx * dy;
                    sum += source.Load(int3(x, y, 0)) * w;
                    area += w;
                }
            }
            c = sum / max(area, 1e-12);
        }
        // Checker background for transparency; float RGB stays unclamped.
        float checker = (((int)floor(xy.x / 12) + (int)floor(xy.y / 12)) % 2) ? 0.08 : 0.14;
        rgb = c.rgb * c.a + checker * (1.0 - c.a);
    }

    // The measurement frame, drawn after the image so it never mixes into a value: a
    // three pixel black band with a one pixel white line down the middle, readable on
    // any content. It is display only - the picker reads the stored pixels.
    if (hasSample > 0.5) {
        float2 outside = max(sampleMin - position.xy, position.xy - sampleMax);
        float edge = max(outside.x, outside.y);
        if (abs(edge) <= 1.5) rgb = float3(0, 0, 0);
        if (abs(edge) <= 0.5) rgb = float3(1, 1, 1);
    }

    rgb *= displayScale;
    if (encodeSrgb > 0.5) rgb = LinearToSrgb(rgb);
    return float4(rgb, 1.0);
}
)";

// Direct3D rejects a constant buffer whose size is not a multiple of 16 bytes, and the
// failure is a returned error rather than anything visible, so the padding is asserted
// rather than left to be counted by eye.
struct ShaderParams {
    float origin[2];
    float zoom;
    float displayScale;
    float imageSize[2];
    float sampleMin[2];
    float sampleMax[2];
    float hasSample;
    float encodeSrgb;
    float pad[4];
};
static_assert(sizeof(ShaderParams) % 16 == 0, "constant buffer size must be a multiple of 16 bytes");

}  // namespace


struct Canvas::Impl {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain1> swapChain;
    com_ptr<ID3D11RenderTargetView> target;
    com_ptr<ID3D11VertexShader> vertexShader;
    com_ptr<ID3D11PixelShader> pixelShader;
    com_ptr<ID3D11Buffer> constants;
    com_ptr<ID3D11ShaderResourceView> textureView;
    com_ptr<ID3D11Texture2D> texture;
    bool scrgb = false;
    UINT width = 0, height = 0;
    ShaderParams params{};
};

Canvas::Canvas() : impl_(std::make_unique<Impl>()) {}
Canvas::~Canvas() = default;

LRESULT CALLBACK Canvas::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    }
    auto* self = reinterpret_cast<Canvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return (LRESULT)RunGuarded("画像表示", [&] { return (long long)self->WndProc(hwnd, msg, wp, lp); });
}

bool Canvas::Create(HWND parent, HINSTANCE instance, std::string& error) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = instance;
        wc.lpszClassName = kCanvasClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hbrBackground = nullptr;
        if (!RegisterClassExW(&wc)) {
            error = "キャンバスのウィンドウクラスを登録できません：" + HresultMessage((long)GetLastError());
            return false;
        }
        registered = true;
    }
    window_ = CreateWindowExW(0, kCanvasClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              0, 0, 100, 100, parent, nullptr, instance, this);
    if (!window_) {
        error = "キャンバスを作成できません：" + HresultMessage((long)GetLastError());
        return false;
    }
    DragAcceptFiles(window_, TRUE);
    if (!CreateDeviceResources(error)) return false;
    RefreshDisplay();
    return true;
}

bool Canvas::CreateDeviceResources(std::string& error) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   (UINT)std::size(levels), D3D11_SDK_VERSION,
                                   impl_->device.put(), nullptr, impl_->context.put());
    if (FAILED(hr)) {
        error = "Direct3D 11デバイスを作成できません：" + HresultMessage((long)hr);
        return false;
    }

    auto dxgiDevice = impl_->device.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(adapter.put());
    com_ptr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.put()));

    RECT client{};
    GetClientRect(window_, &client);
    impl_->width = (UINT)std::max<LONG>(1, client.right - client.left);
    impl_->height = (UINT)std::max<LONG>(1, client.bottom - client.top);

    // Float16 is what carries values above SDR white to the panel. The flip model is
    // what allows this format on a windowed swap chain at all.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = impl_->width;
    desc.Height = impl_->height;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(impl_->device.get(), window_, &desc, nullptr, nullptr,
                                         impl_->swapChain.put());
    if (FAILED(hr)) {
        error = "HDRスワップチェーンを作成できません：" + HresultMessage((long)hr);
        return false;
    }

    if (auto swapChain3 = impl_->swapChain.try_as<IDXGISwapChain3>()) {
        UINT support = 0;
        // scRGB: linear Rec.709 where 1.0 is 80 nit. Values are scaled into it at draw
        // time so that a stored 1.0 lands on the display's SDR white.
        if (SUCCEEDED(swapChain3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support))
            && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
            impl_->scrgb = SUCCEEDED(swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
        }
    }

    com_ptr<ID3DBlob> vertexBlob, pixelBlob, errors;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "HDRScope", nullptr, nullptr,
                    "VSMain", "vs_4_0", 0, 0, vertexBlob.put(), errors.put());
    if (FAILED(hr)) {
        error = std::string("頂点シェーダをコンパイルできません：")
              + (errors ? (const char*)errors->GetBufferPointer() : "");
        return false;
    }
    errors = nullptr;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "HDRScope", nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, pixelBlob.put(), errors.put());
    if (FAILED(hr)) {
        error = std::string("ピクセルシェーダをコンパイルできません：")
              + (errors ? (const char*)errors->GetBufferPointer() : "");
        return false;
    }
    impl_->device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                      nullptr, impl_->vertexShader.put());
    impl_->device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                                     nullptr, impl_->pixelShader.put());

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(ShaderParams);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT bufferResult = impl_->device->CreateBuffer(&constants, nullptr, impl_->constants.put());
    if (FAILED(bufferResult) || !impl_->constants) {
        error = "シェーダ定数バッファを作成できません：" + HresultMessage((long)bufferResult);
        return false;
    }

    ResizeSwapChain();
    return true;
}

void Canvas::ResizeSwapChain() {
    if (!impl_->swapChain) return;
    RECT client{};
    GetClientRect(window_, &client);
    UINT width = (UINT)std::max<LONG>(1, client.right - client.left);
    UINT height = (UINT)std::max<LONG>(1, client.bottom - client.top);
    if (width == impl_->width && height == impl_->height && impl_->target) return;
    impl_->target = nullptr;
    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    impl_->width = width;
    impl_->height = height;
    com_ptr<ID3D11Texture2D> back;
    if (SUCCEEDED(impl_->swapChain->GetBuffer(0, IID_PPV_ARGS(back.put()))))
        impl_->device->CreateRenderTargetView(back.get(), nullptr, impl_->target.put());
}

void Canvas::UploadTexture() {
    impl_->textureView = nullptr;
    impl_->texture = nullptr;
    if (!image_) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)image_->Width();
    desc.Height = (UINT)image_->Height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = image_->Pixels().data();
    data.SysMemPitch = (UINT)image_->Width() * 16;
    if (FAILED(impl_->device->CreateTexture2D(&desc, &data, impl_->texture.put()))) {
        if (onStatus) onStatus("画像のGPUテクスチャを確保できません。画像が大きすぎる可能性があります。");
        return;
    }
    impl_->device->CreateShaderResourceView(impl_->texture.get(), nullptr, impl_->textureView.put());
}

void Canvas::SetImage(PixelImagePtr image, bool preserveView) {
    image_ = std::move(image);
    UploadTexture();
    if (!preserveView) {
        fitMode_ = true;
        Fit();
    } else if (fitMode_) {
        Fit();
    }
    UpdateSample();
    RefreshDisplay();
    Redraw();
}

void Canvas::Fit() {
    if (!image_ || impl_->width == 0 || impl_->height == 0) return;
    zoom_ = std::max(0.01, std::min((double)(impl_->width - 24) / image_->Width(),
                                    (double)(impl_->height - 24) / image_->Height()));
    originX_ = (impl_->width - image_->Width() * zoom_) / 2.0;
    originY_ = (impl_->height - image_->Height() * zoom_) / 2.0;
    ViewChanged();
}

void Canvas::ZoomToActualPixels() {
    fitMode_ = false;
    POINT centre{(LONG)(impl_->width / 2), (LONG)(impl_->height / 2)};
    SetZoom(1.0, centre);
}

void Canvas::SetZoom(double value, POINT anchor) {
    double next = std::min(128.0, std::max(0.01, value));
    originX_ = anchor.x - (anchor.x - originX_) * next / zoom_;
    originY_ = anchor.y - (anchor.y - originY_) * next / zoom_;
    zoom_ = next;
    ViewChanged();
}

void Canvas::ViewChanged() {
    if (onZoom) onZoom(Format("%.1f%%", zoom_ * 100));
    UpdateSample();
    Redraw();
}

void Canvas::Redraw() {
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void Canvas::RefreshDisplay() {
    display_ = QueryDisplayForWindow(window_);

    double headroom = display_.Headroom();
    std::string note;
    if (!display_.valid) {
        note = " · ディスプレイ情報を取得できません";
    } else if (!display_.hdrEnabled) {
        note = display_.hdrCapable ? " · 今はSDR表示です（設定でHDRをONにしてください）"
                                   : " · このディスプレイはHDR表示に対応していません";
    }
    if (!impl_->scrgb) note += " · scRGBスワップチェーン非対応のためSDR表示です";
    // Raising the SDR brightness eats the headroom, and the image then saturates on
    // screen while the data is untouched. Say which of the two the user is seeing.
    if (hasImageMaxRGB && imageMaxRGB > headroom + 0.01) {
        note += Format(" · 画像の最大 %.2f× は表示しきれず頭打ちです。データは保持されています。"
                       "（SDRコンテンツの明るさを下げるとヘッドルームが増えます）", imageMaxRGB);
    }
    // The sustained full-screen limit only earns its place when it differs from the
    // peak; on most panels the two are reported the same and repeating it is noise.
    std::string sustained = display_.maxFullFrameNits > 0
                         && std::abs(display_.maxFullFrameNits - display_.maxLuminanceNits) > 1
        ? Format(" / 全画面 %.0f nit", display_.maxFullFrameNits) : std::string();
    std::string text = display_.valid
        ? Format("表示 %.2f× · パネル %.0f nit%s · SDR白 %.0f nit%s",
                 headroom, display_.maxLuminanceNits, sustained.c_str(),
                 display_.sdrWhiteNits, note.c_str())
        : "表示 不明" + note;
    if (text != lastReadout_) {
        lastReadout_ = text;
        if (onDisplayReadout) onDisplayReadout(text);
    }
    Redraw();
}

void Canvas::UpdateSample() {
    if (!image_ || !cursor_) {
        copyable_.reset();
        impl_->params.hasSample = 0;
        return;
    }
    double px = (cursor_->x - originX_) / zoom_;
    double py = (cursor_->y - originY_) / zoom_;
    if (px < 0 || py < 0 || px >= image_->Width() || py >= image_->Height()) {
        copyable_.reset();
        impl_->params.hasSample = 0;
        if (onSample) onSample("画像の範囲外", "拡張リニアsRGB · 1.0 = SDR白");
        return;
    }
    RectD rect;
    if (zoom_ >= 1) {
        rect = RectD{std::floor(px), std::floor(py), 1, 1};
    } else {
        // The actual screen pixel cell, not a radius centred on a point.
        rect = RectD{(std::floor((double)cursor_->x) - originX_) / zoom_,
                     (std::floor((double)cursor_->y) - originY_) / zoom_,
                     1 / zoom_, 1 / zoom_};
    }
    auto sample = image_->SampleRect(rect);
    if (!sample) {
        copyable_.reset();
        impl_->params.hasSample = 0;
        return;
    }
    impl_->params.hasSample = 1;
    impl_->params.sampleMin[0] = (float)(originX_ + sample->rect.MinX() * zoom_) - 1.0f;
    impl_->params.sampleMin[1] = (float)(originY_ + sample->rect.MinY() * zoom_) - 1.0f;
    impl_->params.sampleMax[0] = (float)(originX_ + sample->rect.MaxX() * zoom_) + 1.0f;
    impl_->params.sampleMax[1] = (float)(originY_ + sample->rect.MaxY() * zoom_) + 1.0f;
    copyable_ = sample->ClipboardText();

    std::string detail = sample->DetailText();
    // Windows composites in scRGB, so the nit value a measurement corresponds to is
    // known exactly. It is the one thing this platform can say that macOS cannot.
    if (display_.valid && display_.hdrEnabled)
        detail += Format(" · %.1f nit 相当", sample->r * 0.2126 * display_.sdrWhiteNits
                                            + sample->g * 0.7152 * display_.sdrWhiteNits
                                            + sample->b * 0.0722 * display_.sdrWhiteNits);
    if (onSample) onSample(sample->Text(), detail);
}

void Canvas::CopySample() {
    if (!copyable_) {
        if (onStatus) onStatus("画像の範囲外です。コピーする測定値がありません。");
        return;
    }
    std::wstring wide = Widen(*copyable_);
    if (!OpenClipboard(window_)) return;
    EmptyClipboard();
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    if (handle) {
        memcpy(GlobalLock(handle), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(handle);
        SetClipboardData(CF_UNICODETEXT, handle);
    }
    CloseClipboard();
    std::string flat = *copyable_;
    size_t at = flat.find("\r\n");
    if (at != std::string::npos) flat.replace(at, 2, " · ");
    if (onStatus) onStatus("クリップボードにコピーしました · " + flat);
}

void Canvas::Render() {
    if (!impl_->target) ResizeSwapChain();
    if (!impl_->target || !impl_->swapChain) return;

    ID3D11RenderTargetView* target = impl_->target.get();
    impl_->context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, (float)impl_->width, (float)impl_->height, 0, 1};
    impl_->context->RSSetViewports(1, &viewport);

    // Values are stored with 1.0 at SDR reference white; scRGB puts 1.0 at 80 nit. This
    // is the factor that lands a stored 1.0 on the display's actual SDR white.
    float displayScale = impl_->scrgb ? (float)display_.ScrgbPerSdrWhite() : 1.0f;
    float background[4] = {0.018f * displayScale, 0.018f * displayScale, 0.022f * displayScale, 1.0f};
    impl_->context->ClearRenderTargetView(target, background);

    if (impl_->textureView && image_ && impl_->constants) {
        impl_->params.origin[0] = (float)originX_;
        impl_->params.origin[1] = (float)originY_;
        impl_->params.zoom = (float)zoom_;
        impl_->params.displayScale = displayScale;
        impl_->params.imageSize[0] = (float)image_->Width();
        impl_->params.imageSize[1] = (float)image_->Height();
        impl_->params.encodeSrgb = impl_->scrgb ? 0.0f : 1.0f;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(impl_->context->Map(impl_->constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &impl_->params, sizeof(ShaderParams));
            impl_->context->Unmap(impl_->constants.get(), 0);
        }
        ID3D11Buffer* constants = impl_->constants.get();
        ID3D11ShaderResourceView* view = impl_->textureView.get();
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->context->IASetInputLayout(nullptr);
        impl_->context->VSSetShader(impl_->vertexShader.get(), nullptr, 0);
        impl_->context->PSSetShader(impl_->pixelShader.get(), nullptr, 0);
        impl_->context->PSSetConstantBuffers(0, 1, &constants);
        impl_->context->PSSetShaderResources(0, 1, &view);
        impl_->context->Draw(3, 0);
    }
    impl_->swapChain->Present(1, 0);
}

LRESULT Canvas::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        ResizeSwapChain();
        if (fitMode_) Fit();
        RefreshDisplay();
        Redraw();
        return 0;
    case WM_DISPLAYCHANGE:
        RefreshDisplay();
        return 0;
    case WM_MOUSEWHEEL: {
        if (!image_) return 0;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &point);
        fitMode_ = false;
        double delta = GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA;
        SetZoom(zoom_ * std::exp(delta * 0.15), point);
        cursor_ = point;
        UpdateSample();
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        dragging_ = true;
        dragPoint_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        dragTravel_ = 0;
        return 0;
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!tracking_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            tracking_ = true;
        }
        if (dragging_) {
            fitMode_ = false;
            originX_ += point.x - dragPoint_.x;
            originY_ += point.y - dragPoint_.y;
            dragTravel_ += std::hypot((double)(point.x - dragPoint_.x), (double)(point.y - dragPoint_.y));
            dragPoint_ = point;
            cursor_ = point;
            ViewChanged();
            return 0;
        }
        cursor_ = point;
        UpdateSample();
        Redraw();
        return 0;
    }
    case WM_LBUTTONUP: {
        bool wasClick = dragging_ && dragTravel_ < kClickSlop;
        dragging_ = false;
        ReleaseCapture();
        if (wasClick) CopySample();
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        cursor_.reset();
        copyable_.reset();
        impl_->params.hasSample = 0;
        if (onSample) onSample("カーソルを画像に重ねるとRGB値を表示します。", "拡張リニアsRGB · 1.0 = SDR白");
        Redraw();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(drop, 0, path, (UINT)std::size(path)) && onFileDropped) onFileDropped(path);
        DragFinish(drop);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
