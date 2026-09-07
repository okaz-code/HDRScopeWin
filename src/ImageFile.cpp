#include "ImageFile.h"
#include "FloatTiff.h"
#include "IccProfile.h"
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using winrt::com_ptr;
using winrt::check_hresult;

double SrgbToLinear(double v) {
    double s = v < 0 ? -1.0 : 1.0;
    double a = std::abs(v);
    return s * (a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4));
}

double LinearToSrgb(double v) {
    double s = v < 0 ? -1.0 : 1.0;
    double a = std::abs(v);
    return s * (a <= 0.0031308 ? a * 12.92 : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055);
}

double PqToLinear(double v) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    double s = v < 0 ? -1.0 : 1.0;
    double e = std::pow(std::abs(v), 1.0 / m2);
    double num = std::max(0.0, e - c1);
    double den = c2 - c3 * e;
    if (den <= 0) return s * 10000.0 / kPqReferenceWhiteNits;
    return s * std::pow(num / den, 1.0 / m1) * 10000.0 / kPqReferenceWhiteNits;
}

double LinearToPq(double v) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    double y = std::clamp(v * kPqReferenceWhiteNits / 10000.0, 0.0, 1.0);
    double p = std::pow(y, m1);
    return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

std::string LoadedImage::FormatText() const {
    return Format("%dbit %s · %s", bitsPerComponent, isFloat ? "float" : "整数", sourceSpace.c_str());
}

namespace {

// The colour space a decoded file is in, in the terms this tool needs: a per-channel
// transfer function plus a matrix onto Rec.709 primaries.
struct SourceSpace {
    enum class Transfer { Srgb, Linear, Pq, IccCurves } transfer = Transfer::Srgb;
    Matrix3 toLinearSRGB{1, 0, 0, 0, 1, 0, 0, 0, 1};
    ToneCurve curves[3];
    // A linear file says which primaries and which curve, but nothing about where
    // reference white sits. When the file does say - a display copy this tool wrote
    // records it in the profile description - this puts the values back on the
    // measurement scale where 1.0 is SDR reference white.
    double valueScale = 1.0;
    std::string name;
    std::string whiteNote;
};

// Reads "1.0 = 203 nit" out of an ICC description. scRGB fixes 1.0 at 80 nit, so a file
// that declares reference white at N nit stored every value multiplied by N/80.
bool ReferenceWhiteFromDescription(const std::string& description, double& nits) {
    size_t at = description.find("1.0 = ");
    if (at == std::string::npos) return false;
    char* end = nullptr;
    double value = strtod(description.c_str() + at + 6, &end);
    if (!end || value <= 0) return false;
    while (*end == ' ') ++end;
    if (strncmp(end, "nit", 3) != 0) return false;
    nits = value;
    return true;
}

com_ptr<IWICImagingFactory> Factory() {
    static com_ptr<IWICImagingFactory> factory = [] {
        com_ptr<IWICImagingFactory> f;
        check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put())));
        return f;
    }();
    return factory;
}

std::wstring LowerExtension(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = (wchar_t)towlower(c);
    return ext;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw ScopeError("ファイルを開けません：" + Narrow(path));
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<uint8_t> data((size_t)size.QuadPart);
    size_t read = 0;
    while (read < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - read, 1u << 24);
        DWORD done = 0;
        if (!ReadFile(file, data.data() + read, chunk, &done, nullptr) || done == 0) break;
        read += done;
    }
    CloseHandle(file);
    data.resize(read);
    return data;
}

// ISOBMFF carries the colour description in a colr box. Rather than walking the whole
// meta/iprp/ipco hierarchy, look for the box header directly: "colr" followed by the
// "nclx" colour type is an eight byte signature, specific enough not to hit by accident.
bool FindNclx(const std::vector<uint8_t>& data, uint16_t& primaries, uint16_t& transfer, uint16_t& matrix) {
    for (size_t i = 0; i + 15 < data.size(); ++i) {
        if (memcmp(data.data() + i, "colrnclx", 8) != 0) continue;
        const uint8_t* p = data.data() + i + 8;
        primaries = (uint16_t)((p[0] << 8) | p[1]);
        transfer  = (uint16_t)((p[2] << 8) | p[3]);
        matrix    = (uint16_t)((p[4] << 8) | p[5]);
        return true;
    }
    return false;
}

Matrix3 PrimariesForCicp(uint16_t code, std::string& name) {
    switch (code) {
    case 1:  name = "BT.709"; return Matrix3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    case 9:  name = "BT.2020";
        return PrimariesToLinearSRGB(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290);
    case 12: name = "Display P3";
        return PrimariesToLinearSRGB(0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290);
    default: name = Format("CICP primaries %u", code); return Matrix3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
}

SourceSpace DetermineSpace(const std::wstring& path, const std::vector<uint8_t>& fileBytes,
                           IWICBitmapFrameDecode* frame, bool sourceIsFloat) {
    SourceSpace space;

    // An embedded ICC profile is the strongest statement the file can make, so it wins.
    UINT contextCount = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &contextCount)) && contextCount > 0) {
        std::vector<com_ptr<IWICColorContext>> contexts(contextCount);
        std::vector<IWICColorContext*> raw(contextCount);
        for (UINT i = 0; i < contextCount; ++i) {
            Factory()->CreateColorContext(contexts[i].put());
            raw[i] = contexts[i].get();
        }
        if (SUCCEEDED(frame->GetColorContexts(contextCount, raw.data(), &contextCount))) {
            for (UINT i = 0; i < contextCount; ++i) {
                WICColorContextType type{};
                if (FAILED(contexts[i]->GetType(&type)) || type != WICColorContextProfile) continue;
                UINT size = 0;
                if (FAILED(contexts[i]->GetProfileBytes(0, nullptr, &size)) || size == 0) continue;
                std::vector<uint8_t> profile(size);
                if (FAILED(contexts[i]->GetProfileBytes(size, profile.data(), &size))) continue;
                MatrixShaper shaper;
                if (!ParseMatrixShaper(profile.data(), profile.size(), shaper)) {
                    space.name = "ICC埋め込み（未対応の形式・sRGBとして解釈）";
                    return space;
                }
                space.transfer = SourceSpace::Transfer::IccCurves;
                for (int c = 0; c < 3; ++c) space.curves[c] = shaper.trc[c];
                space.toLinearSRGB = Multiply(XYZD50ToLinearSRGB(), shaper.toXYZD50);
                space.name = shaper.description.empty() ? "ICC埋め込み" : "ICC: " + shaper.description;
                double declaredWhite = 0;
                if (ReferenceWhiteFromDescription(shaper.description, declaredWhite)) {
                    space.valueScale = 80.0 / declaredWhite;
                    space.whiteNote = Format("このファイルは1.0 = %.0f nitで書かれています。"
                                             "SDR白基準へ戻して測定しました。", declaredWhite);
                }
                return space;
            }
        }
    }

    uint16_t primaries = 0, transfer = 0, matrix = 0;
    if (FindNclx(fileBytes, primaries, transfer, matrix)) {
        std::string primaryName;
        space.toLinearSRGB = PrimariesForCicp(primaries, primaryName);

        // A HEIF decoder that hands back a floating point frame has already undone the
        // transfer function and moved the primaries to Rec.709, because that is what the
        // float pixel formats mean on Windows: scRGB. The CICP then describes how the
        // file was encoded, not what arrived, and applying it again is what turns a PQ
        // image into astronomical numbers. Measured on a PQ HEIC written by the macOS
        // build: the decoded frame contains negative values, which PQ code values cannot
        // be, and its ladder is a constant multiple of the original linear one.
        if (sourceIsFloat) {
            space.transfer = SourceSpace::Transfer::Linear;
            space.toLinearSRGB = Matrix3{1, 0, 0, 0, 1, 0, 0, 0, 1};
            if (transfer == 16) {
                // scRGB fixes 1.0 at 80 nit, and PQ is absolute, so putting it on this
                // tool's scale is a choice of reference white. BT.2408 says 203 nit.
                space.valueScale = 80.0 / kPqReferenceWhiteNits;
                space.name = "BT.2100 PQ / " + primaryName + "（デコーダがscRGBへ変換済み）";
                space.whiteNote = Format("PQは絶対輝度です。1.0 = SDR白 = %.0f nit（BT.2408）として換算しました。",
                                         kPqReferenceWhiteNits);
            } else {
                space.name = "リニアRec.709（" + primaryName + " をデコーダがscRGBへ変換済み）";
                space.whiteNote = "scRGBとして読みました。scRGBの1.0は80 nitで、SDR基準白ではありません。";
            }
            return space;
        }

        if (transfer == 16) {
            space.transfer = SourceSpace::Transfer::Pq;
            space.name = "BT.2100 PQ / " + primaryName;
            space.whiteNote = Format("PQは絶対輝度です。1.0 = SDR白 = %.0f nit（BT.2408）として換算しました。",
                                     kPqReferenceWhiteNits);
        } else if (transfer == 8 || transfer == 13) {
            space.transfer = transfer == 8 ? SourceSpace::Transfer::Linear : SourceSpace::Transfer::Srgb;
            space.name = (transfer == 8 ? "リニア / " : "sRGB / ") + primaryName;
        } else {
            space.transfer = SourceSpace::Transfer::Srgb;
            space.name = Format("CICP transfer %u（sRGBとして解釈）/ ", transfer) + primaryName;
        }
        return space;
    }

    // A float file with no profile is scRGB, which fixes 1.0 at 80 nit and says nothing
    // about where reference white sits. A display copy this tool wrote records that in
    // ImageDescription, because a profile there would make viewers clip it to SDR.
    std::wstring ext = LowerExtension(path);
    if (sourceIsFloat) {
        com_ptr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(frame->GetMetadataQueryReader(metadata.put())) && metadata) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(metadata->GetMetadataByName(L"/ifd/{ushort=270}", &value))) {
                std::string description = value.vt == VT_LPSTR && value.pszVal ? value.pszVal
                                        : (value.vt == VT_LPWSTR && value.pwszVal
                                               ? Narrow(value.pwszVal) : std::string());
                double declaredWhite = 0;
                if (ReferenceWhiteFromDescription(description, declaredWhite)) {
                    space.transfer = SourceSpace::Transfer::Linear;
                    space.valueScale = 80.0 / declaredWhite;
                    space.name = "リニアRec.709 · " + description;
                    space.whiteNote = Format("このファイルは1.0 = %.0f nitで書かれています。"
                                             "SDR白基準へ戻して測定しました。", declaredWhite);
                    PropVariantClear(&value);
                    return space;
                }
            }
            PropVariantClear(&value);
        }
        // A float file with no profile is scRGB by Windows convention: linear Rec.709
        // where 1.0 is 80 nit, which is what an HDR screenshot from the OS contains.
        space.transfer = SourceSpace::Transfer::Linear;
        space.name = (ext == L"jxr" || ext == L"wdp" || ext == L"hdp")
                         ? "scRGB（リニアRec.709・プロファイルなし）"
                         : "リニアRec.709（プロファイルなし）";
        space.whiteNote = "プロファイルがないためscRGBとして読みました。scRGBの1.0は80 nitで、"
                          "SDR基準白ではありません。値×80 = nitです。";
        return space;
    }
    space.transfer = SourceSpace::Transfer::Srgb;
    space.name = "sRGB（プロファイルなし）";
    return space;
}

bool IsFloatFormat(const GUID& fmt) {
    return fmt == GUID_WICPixelFormat128bppRGBAFloat || fmt == GUID_WICPixelFormat128bppPRGBAFloat
        || fmt == GUID_WICPixelFormat128bppRGBFloat || fmt == GUID_WICPixelFormat96bppRGBFloat
        || fmt == GUID_WICPixelFormat64bppRGBAHalf || fmt == GUID_WICPixelFormat64bppPRGBAHalf
        || fmt == GUID_WICPixelFormat64bppRGBHalf || fmt == GUID_WICPixelFormat48bppRGBHalf
        || fmt == GUID_WICPixelFormat32bppGrayFloat;
}

bool IsPremultiplied(const GUID& fmt) {
    return fmt == GUID_WICPixelFormat128bppPRGBAFloat || fmt == GUID_WICPixelFormat64bppPRGBAHalf
        || fmt == GUID_WICPixelFormat32bppPBGRA || fmt == GUID_WICPixelFormat32bppPRGBA
        || fmt == GUID_WICPixelFormat64bppPRGBA;
}

int BitsOf(const GUID& fmt, IWICImagingFactory* factory) {
    com_ptr<IWICComponentInfo> info;
    if (FAILED(factory->CreateComponentInfo(fmt, info.put()))) return 8;
    auto pixelInfo = info.try_as<IWICPixelFormatInfo>();
    if (!pixelInfo) return 8;
    UINT bpp = 0, channels = 0;
    pixelInfo->GetBitsPerPixel(&bpp);
    pixelInfo->GetChannelCount(&channels);
    return channels ? (int)(bpp / channels) : 8;
}

}  // namespace

namespace ImageFile {

const wchar_t* FilterSpec() {
    return L"対応画像 (*.tiff;*.tif;*.jxr;*.heic;*.heif;*.png;*.jpg;*.jpeg)\0"
           L"*.tiff;*.tif;*.jxr;*.heic;*.heif;*.png;*.jpg;*.jpeg\0"
           L"すべてのファイル (*.*)\0*.*\0\0";
}

bool CanRead(const std::wstring& path) {
    std::wstring ext = LowerExtension(path);
    static const wchar_t* known[] = {L"tiff", L"tif", L"jxr", L"wdp", L"hdp",
                                     L"heic", L"heif", L"png", L"jpg", L"jpeg"};
    for (const wchar_t* k : known)
        if (ext == k) return true;
    return false;
}

LoadedImage Load(const std::wstring& path) {
    std::vector<uint8_t> fileBytes = ReadFileBytes(path);
    if (fileBytes.empty()) throw ScopeError("ファイルが空です：" + Narrow(path));

    LoadedImage result;
    int width = 0, height = 0;
    std::vector<float> pixels;

    com_ptr<IWICBitmapDecoder> decoder;
    HRESULT hr = Factory()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, decoder.put());
    com_ptr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, frame.put());

    if (FAILED(hr)) {
        // The platform decoder turns down some float TIFFs. Ours has a known layout, so
        // read it directly rather than telling the user the file is unreadable.
        if (auto tiff = FloatTiff::Decode(fileBytes.data(), fileBytes.size())) {
            auto image = std::make_shared<PixelImage>(tiff->width, tiff->height, std::move(tiff->pixels));
            result.image = image;
            result.summary = image->Summary();
            result.sourceSpace = "リニアsRGB（HDRScopeのfloat TIFF・独自リーダー）";
            result.bitsPerComponent = 32;
            result.isFloat = true;
            return result;
        }
        throw ScopeError("画像として読み込めません：" + Narrow(path) + " · " + HresultMessage((long)hr));
    }

    UINT w = 0, h = 0;
    check_hresult(frame->GetSize(&w, &h));
    if (w == 0 || h == 0) throw ScopeError("画像の寸法が不正です。");
    width = (int)w; height = (int)h;

    GUID sourceFormat{};
    check_hresult(frame->GetPixelFormat(&sourceFormat));
    bool sourceIsFloat = IsFloatFormat(sourceFormat);
    bool premultiplied = IsPremultiplied(sourceFormat);
    result.bitsPerComponent = BitsOf(sourceFormat, Factory().get());
    result.isFloat = sourceIsFloat;

    SourceSpace space = DetermineSpace(path, fileBytes, frame.get(), sourceIsFloat);
    result.sourceSpace = space.name;
    result.whiteNote = space.whiteNote;

    pixels.resize((size_t)width * height * 4);
    if (sourceIsFloat) {
        // Float to float is a widening, not a colour conversion, so the encoded values
        // arrive untouched and the transfer function below is the only one applied.
        com_ptr<IWICFormatConverter> converter;
        check_hresult(Factory()->CreateFormatConverter(converter.put()));
        check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat128bppRGBAFloat,
                                            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
        check_hresult(converter->CopyPixels(nullptr, (UINT)width * 16,
                                            (UINT)(pixels.size() * 4), (BYTE*)pixels.data()));
        premultiplied = false;  // 128bppRGBAFloat is straight alpha
    } else {
        // Integer formats stay integer until the transfer function runs here, so no
        // part of the platform stack gets to linearize with its own idea of the curve.
        std::vector<uint16_t> raw((size_t)width * height * 4);
        com_ptr<IWICFormatConverter> converter;
        check_hresult(Factory()->CreateFormatConverter(converter.put()));
        check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat64bppRGBA,
                                            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
        check_hresult(converter->CopyPixels(nullptr, (UINT)width * 8,
                                            (UINT)(raw.size() * 2), (BYTE*)raw.data()));
        for (size_t i = 0; i < raw.size(); ++i) pixels[i] = raw[i] / 65535.0f;
        premultiplied = false;  // 64bppRGBA is straight alpha
    }

    // ICC stores colorants as s15Fixed16, so no profile can express sRGB primaries
    // exactly and a matrix built from one lands a few parts in 100000 off identity.
    // Multiplying by it would perturb every value of a file that is already in the
    // measurement space, so a matrix this close is treated as the identity it means.
    // The nearest real alternative, Display P3, is 22% away, far outside this window.
    const Matrix3& m = space.toLinearSRGB;
    const double kIdentityTolerance = 1e-3;
    bool identityMatrix = true;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (std::abs(m[r * 3 + c] - (r == c ? 1.0 : 0.0)) > kIdentityTolerance) identityMatrix = false;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        double rgb[3];
        for (int c = 0; c < 3; ++c) {
            double v = pixels[i + c];
            switch (space.transfer) {
            case SourceSpace::Transfer::Srgb:      rgb[c] = SrgbToLinear(v); break;
            case SourceSpace::Transfer::Linear:    rgb[c] = v; break;
            case SourceSpace::Transfer::Pq:        rgb[c] = PqToLinear(v); break;
            case SourceSpace::Transfer::IccCurves: rgb[c] = space.curves[c].ToLinear(v); break;
            }
        }
        if (!identityMatrix) {
            double out[3];
            for (int r = 0; r < 3; ++r)
                out[r] = m[r * 3] * rgb[0] + m[r * 3 + 1] * rgb[1] + m[r * 3 + 2] * rgb[2];
            rgb[0] = out[0]; rgb[1] = out[1]; rgb[2] = out[2];
        }
        double alpha = pixels[i + 3];
        if (premultiplied && alpha > 0 && alpha != 1)
            for (int c = 0; c < 3; ++c) rgb[c] /= alpha;
        for (int c = 0; c < 3; ++c) pixels[i + c] = (float)(rgb[c] * space.valueScale);
    }

    for (float v : pixels)
        if (!std::isfinite(v)) throw ScopeError("画像に非有限の画素値が含まれています。");

    auto image = std::make_shared<PixelImage>(width, height, std::move(pixels));
    result.image = image;
    result.summary = image->Summary();
    return result;
}

}  // namespace ImageFile
