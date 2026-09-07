#include "HdrExport.h"
#include "FloatTiff.h"
#include "IccProfile.h"
#include "ImageFile.h"
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <DirectXPackedVector.h>
#include <winrt/base.h>
#include <algorithm>
#include <vector>

using winrt::com_ptr;
using winrt::check_hresult;

namespace {

com_ptr<IWICImagingFactory> Factory() {
    com_ptr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.put())));
    return factory;
}

std::wstring TempSibling(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    GUID id{};
    CoCreateGuid(&id);
    wchar_t name[64];
    swprintf_s(name, L"\\.HDRScope-%08lX%04X%04X.tmp", id.Data1, id.Data2, id.Data3);
    return dir + name;
}

// A display copy is written on the BT.2408 reference white so that it looks the way it
// is meant to look in a viewer, rather than on this tool's measurement scale. scRGB
// fixes 1.0 at 80 nit, so SDR white has to be written at 203/80 to land at 203 nit.
constexpr double kDisplayScale = kPqReferenceWhiteNits / 80.0;

void SaveHdrJxr(const PixelImage& image, const std::wstring& path) {
    std::wstring staging = TempSibling(path);
    auto factory = Factory();
    {
        com_ptr<IWICStream> stream;
        check_hresult(factory->CreateStream(stream.put()));
        check_hresult(stream->InitializeFromFilename(staging.c_str(), GENERIC_WRITE));
        com_ptr<IWICBitmapEncoder> encoder;
        HRESULT hr = factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, encoder.put());
        if (FAILED(hr))
            throw ScopeError("JPEG XRエンコーダを利用できません：" + HresultMessage((long)hr));
        check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        com_ptr<IWICBitmapFrameEncode> frame;
        com_ptr<IPropertyBag2> options;
        check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
        if (options) {
            // Lossless, so the only thing between the data and the file is the half
            // float rounding this format is built on.
            PROPBAG2 lossless{};
            lossless.pstrName = const_cast<wchar_t*>(L"Lossless");
            VARIANT value{};
            value.vt = VT_BOOL;
            value.boolVal = VARIANT_TRUE;
            options->Write(1, &lossless, &value);
        }
        check_hresult(frame->Initialize(options.get()));
        check_hresult(frame->SetSize((UINT)image.Width(), (UINT)image.Height()));
        GUID format = GUID_WICPixelFormat64bppRGBAHalf;
        check_hresult(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat64bppRGBAHalf)
            throw ScopeError("JPEG XRエンコーダが16bit floatを受け付けませんでした。");

        // No ICC profile here, deliberately. A colour managed viewer that finds one
        // treats the file as ordinary linear sRGB and clips it to SDR - measured: with a
        // profile Windows Photos peaks at exactly SDR white, without one it renders the
        // full range. An untagged float JPEG XR is scRGB by Windows convention, which is
        // what this is, so the profile adds nothing a viewer needs and costs the HDR.
        //
        // What 1.0 means still has to be recorded somewhere, or this tool cannot read its
        // own display copy back on the scale it wrote it from. ImageDescription carries it:
        // metadata no viewer colour manages on.
        com_ptr<IWICMetadataQueryWriter> metadata;
        if (SUCCEEDED(frame->GetMetadataQueryWriter(metadata.put())) && metadata) {
            std::string description = DisplayCopyDescription(kPqReferenceWhiteNits);
            PROPVARIANT value{};
            value.vt = VT_LPSTR;
            value.pszVal = const_cast<char*>(description.c_str());
            metadata->SetMetadataByName(L"/ifd/{ushort=270}", &value);
        }

        std::vector<uint16_t> halves((size_t)image.Width() * image.Height() * 4);
        const std::vector<float>& pixels = image.Pixels();
        for (size_t i = 0; i < halves.size(); i += 4) {
            for (int c = 0; c < 3; ++c)
                halves[i + c] = DirectX::PackedVector::XMConvertFloatToHalf((float)(pixels[i + c] * kDisplayScale));
            halves[i + 3] = DirectX::PackedVector::XMConvertFloatToHalf(pixels[i + 3]);
        }
        UINT stride = (UINT)image.Width() * 8;
        check_hresult(frame->WritePixels((UINT)image.Height(), stride,
                                         (UINT)(halves.size() * 2), (BYTE*)halves.data()));
        check_hresult(frame->Commit());
        check_hresult(encoder->Commit());
    }

    // Read the staged file back before it replaces anything: a failed encode must not
    // leave a half written file where a previous good one was.
    {
        com_ptr<IWICBitmapDecoder> decoder;
        HRESULT hr = factory->CreateDecoderFromFilename(staging.c_str(), nullptr, GENERIC_READ,
                                                        WICDecodeMetadataCacheOnDemand, decoder.put());
        com_ptr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, frame.put());
        UINT w = 0, h = 0;
        GUID format{};
        if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
        if (SUCCEEDED(hr)) hr = frame->GetPixelFormat(&format);
        bool ok = SUCCEEDED(hr) && (int)w == image.Width() && (int)h == image.Height()
               && format == GUID_WICPixelFormat64bppRGBAHalf;
        decoder = nullptr;
        frame = nullptr;
        if (!ok) {
            DeleteFileW(staging.c_str());
            throw ScopeError("保存したJPEG XRの読み戻し検証に失敗しました。");
        }
    }

    if (!MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(staging.c_str());
        throw ScopeError("保存先へ差し替えられません：" + HresultMessage((long)err));
    }
}

const SaveFormatInfo kTiff{
    L"tiff",
    "32bit float TIFF（測定用・数値そのまま）",
    "32bit float TIFF・拡張リニアsRGB・HDR値をクリップせず保持",
    "TIFF保存・読み戻し検証中…",
    "保存・読み戻し検証完了",
    L"32bit float TIFF (*.tiff)\0*.tiff\0すべてのファイル (*.*)\0*.*\0\0",
};

const SaveFormatInfo kJxr{
    L"jxr",
    "HDR JPEG XR（表示用・フォトでHDR）",
    "16bit float scRGB JPEG XR・Windowsフォトの HDR 表示用・値は表示基準へスケールされます",
    "JPEG XR保存・読み戻し検証中…",
    "HDR JPEG XR保存完了（表示用・1.0 = 203 nit へスケール。測定にはTIFFを使ってください）",
    L"HDR JPEG XR (*.jxr)\0*.jxr\0すべてのファイル (*.*)\0*.*\0\0",
};

}  // namespace

const SaveFormatInfo& FormatInfo(SaveFormat format) {
    return format == SaveFormat::FloatTiff ? kTiff : kJxr;
}

void SaveImage(const PixelImage& image, SaveFormat format, const std::wstring& path) {
    if (format == SaveFormat::FloatTiff) FloatTiff::Save(image, path);
    else SaveHdrJxr(image, path);
}
