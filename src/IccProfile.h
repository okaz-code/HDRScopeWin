#pragma once
#include "Common.h"
#include <array>
#include <cstdint>
#include <vector>

// A 3x3 matrix in row-major order, applied as out = M * in.
using Matrix3 = std::array<double, 9>;

// One channel's tone response curve, as ICC stores it. Identity, a single gamma
// exponent, a sampled table, or the parametric form.
struct ToneCurve {
    enum class Kind { Identity, Gamma, Table, Parametric } kind = Kind::Identity;
    double gamma = 1.0;
    std::vector<float> table;          // Kind::Table, normalized 0..1, evenly spaced
    int parametricType = 0;            // Kind::Parametric, ICC parametricCurveType
    double params[7] = {0, 0, 0, 0, 0, 0, 0};

    // Encoded value to linear. Values outside 0..1 are extrapolated with odd symmetry
    // so an extended-range file keeps its sign instead of being clamped to black.
    double ToLinear(double v) const;
};

// The matrix/TRC ("matrix shaper") profile shape: the one every RGB display profile of
// interest uses - sRGB, linear sRGB, Display P3, Adobe RGB, Rec.2020.
struct MatrixShaper {
    Matrix3 toXYZD50{};
    ToneCurve trc[3];
    std::string description;
};

// Reads the tags this tool needs and reports failure for anything else (LUT-based
// profiles, CMYK, v4 A2B pipelines) rather than silently guessing a conversion.
bool ParseMatrixShaper(const uint8_t* data, size_t size, MatrixShaper& out);

// The profile embedded in the files this tool writes: sRGB primaries, D65 white,
// gamma 1.0. A reader that honours it gets the values back unchanged.
//
// The description is not decoration. A profile says which primaries and which curve, but
// nothing about where reference white sits, and a display copy is written on a different
// white than the measurement scale. Stating the white in the description is what lets
// this tool read its own display copies back at the value it wrote them from.
std::string LinearSRGBDescription();
std::string DisplayCopyDescription(double referenceWhiteNits);
std::vector<uint8_t> BuildLinearSRGBProfile(const std::string& description = LinearSRGBDescription());

// Colour space conversion targets. Everything the app measures lives in linear sRGB
// (Rec.709 primaries, D65), which is what the extended-range values mean.
Matrix3 Multiply(const Matrix3& a, const Matrix3& b);
Matrix3 Invert(const Matrix3& m);
// Linear sRGB and the ICC D50 profile connection space, in both directions. The two
// are exact inverses, so a profile this tool writes reads back as the same space.
const Matrix3& LinearSRGBToXYZD50();
const Matrix3& XYZD50ToLinearSRGB();
// Named primaries to linear sRGB, for files that carry CICP codes instead of a profile.
Matrix3 PrimariesToLinearSRGB(double rx, double ry, double gx, double gy,
                              double bx, double by, double wx, double wy);
