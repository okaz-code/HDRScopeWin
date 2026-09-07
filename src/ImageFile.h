#pragma once
#include "PixelImage.h"
#include <string>
#include <vector>

// Reading a saved image back. Whatever the file's own colour space is - linear sRGB for
// the TIFF this tool writes, scRGB for a Windows HDR screenshot, BT.2100 PQ for a HEIC
// from the macOS build - it is converted to the same extended linear sRGB the picker
// measures in, so an opened file is measured on the capture's terms.
struct LoadedImage {
    PixelImagePtr image;
    ContentSummary summary;
    std::string sourceSpace;       // what the file said it was
    std::string whiteNote;         // what 1.0 means in this file, when that is not obvious
    int bitsPerComponent = 8;
    bool isFloat = false;
    std::string FormatText() const;
};

namespace ImageFile {

bool CanRead(const std::wstring& path);
LoadedImage Load(const std::wstring& path);
// Extensions offered in the open dialog, in the order they appear there.
const wchar_t* FilterSpec();

}  // namespace ImageFile

// Transfer functions, exposed because the self-tests check them directly. Both are
// extended with odd symmetry so a negative encoded value keeps its sign rather than
// clamping to black - that is what makes an out-of-gamut colour survive a round trip.
double SrgbToLinear(double v);
double LinearToSrgb(double v);
// BT.2100 PQ decodes to absolute luminance. Dividing by the BT.2408 reference white is
// what puts it on this tool's scale, where 1.0 is SDR reference white.
inline constexpr double kPqReferenceWhiteNits = 203.0;
double PqToLinear(double v);
double LinearToPq(double v);
