#include "SelfTests.h"
#include "FloatTiff.h"
#include "IccProfile.h"
#include "HdrExport.h"
#include "ImageFile.h"
#include "PixelImage.h"
#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

using winrt::com_ptr;
using winrt::check_hresult;

namespace {

int g_assertions = 0;

void Check(bool ok, const std::string& message) {
    ++g_assertions;
    if (!ok) throw ScopeError("TEST FAILED: " + message);
}

bool Near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

void WriteSrgbPng(const std::wstring& path) {
    com_ptr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())));
    com_ptr<IWICStream> stream;
    check_hresult(factory->CreateStream(stream.put()));
    check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    com_ptr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize(2, 1));
    GUID format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    // BGRA: mid grey then opaque white, both plain sRGB with no profile.
    uint8_t pixels[8] = {128, 128, 128, 255, 255, 255, 255, 255};
    check_hresult(frame->WritePixels(1, 8, 8, pixels));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());
}

}  // namespace

int RunSelfTests(const std::wstring& directory) {
    g_assertions = 0;
    CreateDirectoryW(directory.c_str(), nullptr);

    // Asymmetric rows detect vertical flips, and include HDR, negative RGB and alpha.
    std::vector<float> values = {0.18f, 1, 8, 1,   -0.25f, 4, 0.125f, 1,
                                 2, 0.5f, 16, 0.5f, 0.0625f, 0.25f, 0.75f, 1};
    PixelImage image(2, 2, values);

    auto top = image.SampleRect(RectD{0, 0, 1, 1});
    Check(top.has_value(), "single pixel sample exists");
    Check(Near(top->r, 0.18, 1e-7) && top->b == 8, "single pixel / SDR reference scale");

    auto mean = image.SampleRect(RectD{0, 0, 2, 2});
    Check(mean.has_value() && Near(mean->r, (values[0] + values[4] + values[8] + values[12]) / 4.0, 1e-7),
          "box mean");

    auto weighted = image.SampleRect(RectD{0.75, 0, 1, 1});
    Check(weighted.has_value() && Near(weighted->r, values[0] * 0.25 + values[4] * 0.75, 1e-10),
          "fractional area weights");

    auto clipped = image.SampleRect(RectD{-1, -1, 2, 2});
    Check(clipped.has_value() && clipped->r == top->r && clipped->g == top->g && clipped->b == top->b,
          "image edge renormalization");
    Check(!image.SampleRect(RectD{3, 0, 1, 1}).has_value(), "outside image");
    Check(image.SampleRect(RectD{0, 1, 2, 1})->r == (values[8] + values[12]) / 2.0f,
          "row order is top to bottom");

    // The two lines a click copies are exactly the two lines shown under the image.
    Check(top->ClipboardText() == top->Text() + "\r\n" + top->DetailText(), "clipboard text is the two shown lines");
    Check(top->Text().find("R: 0.180000") == 0, "sample formatting");
    Check(top->DetailText().find("1画素") != std::string::npos, "single pixel is not called an average");
    Check(image.SampleRect(RectD{0, 0, 2, 2})->DetailText().find("面積加重平均") != std::string::npos,
          "multi pixel is called an area weighted mean");

    // Transfer functions, extended with odd symmetry past both ends of 0..1.
    Check(Near(SrgbToLinear(0.5), 0.21404114, 2e-7), "sRGB transfer decoding");
    Check(Near(SrgbToLinear(-0.5), -0.21404114, 2e-7), "sRGB transfer keeps negative sign");
    Check(Near(SrgbToLinear(1.0), 1.0, 1e-12), "sRGB white maps to 1.0");
    Check(Near(LinearToSrgb(SrgbToLinear(0.73)), 0.73, 1e-9), "sRGB transfer round trip");
    Check(Near(PqToLinear(LinearToPq(4.0)), 4.0, 1e-6), "PQ round trip at 4x SDR white");
    Check(Near(PqToLinear(LinearToPq(1.0)), 1.0, 1e-6), "PQ round trip at SDR white");

    // The gamut conversion table printed in the README, recomputed from the primaries.
    Matrix3 p3 = PrimariesToLinearSRGB(0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290);
    Check(Near(p3[0], 1.2249, 5e-4) && Near(p3[3], -0.0421, 5e-4) && Near(p3[6], -0.0196, 5e-4),
          Format("Display P3 red matches the README table: %.4f %.4f %.4f", p3[0], p3[3], p3[6]));
    Check(Near(p3[1], -0.2249, 5e-4) && Near(p3[4], 1.0421, 5e-4) && Near(p3[7], -0.0786, 5e-4),
          "Display P3 green matches the README table");
    Matrix3 bt2020 = PrimariesToLinearSRGB(0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290);
    Check(Near(bt2020[0], 1.6605, 5e-4) && Near(bt2020[3], -0.1246, 5e-4) && Near(bt2020[6], -0.0182, 5e-4),
          Format("Rec.2020 red matches the README table: %.4f %.4f %.4f", bt2020[0], bt2020[3], bt2020[6]));
    Check(Near(bt2020[1], -0.5876, 5e-4) && Near(bt2020[4], 1.1329, 5e-4) && Near(bt2020[7], -0.1006, 5e-4),
          "Rec.2020 green matches the README table");

    // The ICC profile written into every saved TIFF has to read back as exactly the
    // space it claims: sRGB primaries, D65, gamma 1.0.
    std::vector<uint8_t> profile = BuildLinearSRGBProfile();
    MatrixShaper shaper;
    Check(ParseMatrixShaper(profile.data(), profile.size(), shaper), "linear sRGB ICC profile parses");
    Check(shaper.trc[0].kind == ToneCurve::Kind::Identity, "embedded TRC is gamma 1.0");
    Matrix3 roundTrip = Multiply(XYZD50ToLinearSRGB(), shaper.toXYZD50);
    Check(Near(roundTrip[0], 1.0, 2e-3) && Near(roundTrip[4], 1.0, 2e-3) && Near(roundTrip[8], 1.0, 2e-3)
          && Near(roundTrip[1], 0.0, 2e-3) && Near(roundTrip[5], 0.0, 2e-3) && Near(roundTrip[6], 0.0, 2e-3),
          Format("embedded profile round trips to identity (diag %.5f %.5f %.5f)",
                 roundTrip[0], roundTrip[4], roundTrip[8]));

    // A float TIFF has to come back bit for bit, including HDR, negative and alpha.
    std::vector<uint8_t> encoded = FloatTiff::Encode(image);
    auto decoded = FloatTiff::Decode(encoded.data(), encoded.size());
    Check(decoded.has_value(), "float TIFF decodes");
    Check(decoded->width == 2 && decoded->height == 2, "float TIFF dimensions");
    Check(decoded->pixels == values, "float TIFF is bit identical");
    Check(decoded->hasIccProfile, "float TIFF carries an ICC profile");
    Check(decoded->description.find("1.0 = SDR reference white") != std::string::npos,
          "float TIFF states its white convention");

    std::wstring tiffPath = directory + L"\\roundtrip.tiff";
    FloatTiff::Save(image, tiffPath);
    LoadedImage reloaded = ImageFile::Load(tiffPath);
    {
        std::string detail;
        for (size_t i = 0; i < 8; ++i)
            detail += Format(" [%zu] %.9g vs %.9g", i, (double)values[i], (double)reloaded.image->Pixels()[i]);
        Check(reloaded.image->Pixels() == values,
              "saved TIFF reloads bit identical through the app reader · space=" + reloaded.sourceSpace + detail);
    }
    Check(reloaded.isFloat && reloaded.bitsPerComponent == 32, "saved TIFF reports 32bit float");
    Check(Near(reloaded.summary.maxRGB, 16.0, 1e-6), "reloaded TIFF keeps its HDR maximum");
    Check(Near(reloaded.summary.minRGB, -0.25, 1e-6), "reloaded TIFF keeps its negative value");

    // Overwriting has to leave a complete file, not a truncated one, so this writes the
    // small image first and then a much larger one over it. roundtrip.tiff is left
    // holding the 2x2 image for Tests/verify_tiff.py to check independently.
    std::wstring patternPath = directory + L"\\pattern.tiff";
    FloatTiff::Save(image, patternPath);
    FloatTiff::Save(*PixelImage::TestPattern(), patternPath);
    LoadedImage pattern = ImageFile::Load(patternPath);
    Check(pattern.image->Width() == (int)PixelImage::TestPatternLevels().size() * 40,
          "test pattern width follows the level count");
    Check(Near(pattern.image->SampleRect(RectD{0, 0, 40, 180})->r, 0.0, 1e-6), "pattern starts at black");
    auto white30 = pattern.image->SampleRect(RectD{(double)(PixelImage::TestPatternLevels().size() - 1) * 40, 0, 40, 180});
    Check(Near(white30->r, 30.0, 1e-5) && Near(white30->g, 30.0, 1e-5) && Near(white30->b, 30.0, 1e-5),
          "pattern reaches 30x SDR white");
    auto redRow = pattern.image->SampleRect(RectD{(double)(PixelImage::TestPatternLevels().size() - 1) * 40, 180, 40, 180});
    Check(Near(redRow->r, 30.0, 1e-5) && Near(redRow->g, 0.0, 1e-6), "red ladder is red only");

    // The integer path: a plain sRGB PNG has to arrive linearized, not relabelled.
    std::wstring pngPath = directory + L"\\srgb.png";
    WriteSrgbPng(pngPath);
    LoadedImage png = ImageFile::Load(pngPath);
    Check(!png.isFloat && png.bitsPerComponent == 8, "PNG reports 8bit integer");
    auto grey = png.image->SampleRect(RectD{0, 0, 1, 1});
    Check(Near(grey->r, SrgbToLinear(128 / 255.0), 1e-4),
          Format("sRGB PNG is linearized on load (got %.6f)", grey->r));
    auto pngWhite = png.image->SampleRect(RectD{1, 0, 1, 1});
    Check(Near(pngWhite->r, 1.0, 1e-4), "sRGB white loads as SDR reference white");

    // The headroom ladder is built for one display, so its shape has to follow the
    // headroom rather than a fixed table. Whatever it picks, SDR white has to land on a
    // step, the steps have to be evenly spaced from zero, and there have to be steps
    // past the headroom or the plateau that marks the limit cannot be seen.
    struct LadderCase { double headroom; float step; size_t columns; };
    const LadderCase ladders[] = {
        {1.00, 0.0625f, 20}, {2.00, 0.125f, 20}, {5.79, 0.25f, 28}, {8.00, 0.5f, 20},
        {12.0, 0.5f, 28}, {20.0, 1.0f, 24}, {30.0, 1.0f, 34},
    };
    for (const LadderCase& expected : ladders) {
        PixelImage::HeadroomLadder ladder = PixelImage::HeadroomLevels(expected.headroom);
        std::string where = Format(" (headroom %.2f -> step %.4g, %zu columns, top %.4g)",
                                   expected.headroom, ladder.step, ladder.levels.size(), ladder.top);
        Check(ladder.step == expected.step, "headroom ladder step" + where);
        Check(ladder.levels.size() == expected.columns, "headroom ladder column count" + where);
        Check(ladder.levels.front() == 0.0f, "headroom ladder starts at black" + where);
        Check(ladder.top >= expected.headroom, "headroom ladder reaches the headroom" + where);
        Check(ladder.reachesHeadroom, "headroom ladder reports reaching it" + where);
        Check(ladder.top - (float)expected.headroom <= 4 * ladder.step,
              "headroom ladder stops within four steps past the headroom" + where);
        Check(std::find(ladder.levels.begin(), ladder.levels.end(), 1.0f) != ladder.levels.end(),
              "SDR white lands exactly on a step" + where);
        for (size_t i = 1; i < ladder.levels.size(); ++i)
            Check(Near(ladder.levels[i] - ladder.levels[i - 1], ladder.step, 1e-6),
                  "headroom ladder steps are evenly spaced" + where);
        Check(ladder.levels.size() <= 34, "headroom ladder fits the column budget" + where);
    }
    // A headroom past the widest whole-step ladder cannot be reached, and says so.
    PixelImage::HeadroomLadder huge = PixelImage::HeadroomLevels(60.0);
    Check(huge.levels.size() == 34 && huge.step == 1.0f && !huge.reachesHeadroom,
          Format("an unreachable headroom is reported, not faked (step %.4g, top %.4g, reaches %d)",
                 huge.step, huge.top, (int)huge.reachesHeadroom));
    // Headroom is never below SDR white, so an SDR display still gets a usable ladder.
    Check(PixelImage::HeadroomLevels(0.5).headroom == 1.0, "headroom below 1.0 is clamped");

    auto headroomImage = PixelImage::Pattern(PixelImage::HeadroomLevels(5.79).levels);
    Check(headroomImage->Width() == 28 * 40, "headroom pattern width follows its column count");
    Check(Near(headroomImage->SampleRect(RectD{4 * 40 + 10, 10, 1, 1})->r, 1.0, 1e-6),
          "headroom pattern puts SDR white where the step index says");
    Check(Near(headroomImage->SampleRect(RectD{27 * 40 + 10, 10, 1, 1})->r, 6.75, 1e-6),
          "headroom pattern tops out at its last step");

    // The display copy is written on the BT.2408 reference white rather than the
    // measurement scale, and says so in its profile, so this tool reads it back at the
    // value it wrote it from. Half float rounding is the only loss allowed.
    std::wstring jxrPath = directory + L"\\display-copy.jxr";
    SaveImage(image, SaveFormat::HdrJxr, jxrPath);
    LoadedImage jxr = ImageFile::Load(jxrPath);
    Check(jxr.image->Width() == 2 && jxr.image->Height() == 2, "JPEG XR dimensions survive");
    Check(jxr.isFloat && jxr.bitsPerComponent == 16, "JPEG XR is 16bit float");
    Check(jxr.sourceSpace.find("203 nit") != std::string::npos,
          "JPEG XR records its reference white · got " + jxr.sourceSpace);
    double worst = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        double got = jxr.image->Pixels()[i], want = values[i];
        double relative = std::abs(got - want) / std::max(1e-3, std::abs(want));
        worst = std::max(worst, relative);
    }
    Check(worst < 2e-3, Format("JPEG XR round trips within half float precision (worst %.6f)", worst));

    // Summary is what tells the user whether a file is really HDR.
    ContentSummary summary = image.Summary();
    Check(Near(summary.maxRGB, 16.0, 1e-6) && Near(summary.minRGB, -0.25, 1e-6), "summary range");
    Check(Near(summary.overOne, 4.0 / 12.0, 1e-9), Format("summary counts values above 1.0 (%.6f)", summary.overOne));
    Check(summary.maxAlpha == 1.0f && !summary.uniform, "summary alpha and uniformity");
    ContentSummary flat = PixelImage(2, 1, {1, 1, 1, 1, 1, 1, 1, 1}).Summary();
    Check(flat.uniform, "a uniform image is reported as uniform");

    printf("%d assertions passed\n", g_assertions);
    printf("output: %ls\n", directory.c_str());
    return 0;
}
