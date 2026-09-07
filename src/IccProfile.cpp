#include "IccProfile.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

uint32_t ReadU32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint16_t ReadU16(const uint8_t* p) { return (uint16_t)(((uint32_t)p[0] << 8) | p[1]); }
double ReadS15Fixed16(const uint8_t* p) { return (double)(int32_t)ReadU32(p) / 65536.0; }

void WriteU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
void WriteTag(std::vector<uint8_t>& v, const char* sig) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)sig[i]);
}
void WriteS15Fixed16(std::vector<uint8_t>& v, double x) {
    WriteU32(v, (uint32_t)(int32_t)std::lround(x * 65536.0));
}

uint32_t Sig(const char* s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3];
}

bool ParseCurve(const uint8_t* data, size_t size, ToneCurve& curve) {
    if (size < 12) return false;
    uint32_t type = ReadU32(data);
    if (type == Sig("curv")) {
        uint32_t count = ReadU32(data + 8);
        if (count == 0) { curve.kind = ToneCurve::Kind::Identity; return true; }
        if (count == 1) {
            if (size < 14) return false;
            curve.kind = ToneCurve::Kind::Gamma;
            curve.gamma = ReadU16(data + 12) / 256.0;   // u8Fixed8
            return true;
        }
        if (size < 12 + (size_t)count * 2) return false;
        curve.kind = ToneCurve::Kind::Table;
        curve.table.resize(count);
        for (uint32_t i = 0; i < count; ++i) curve.table[i] = ReadU16(data + 12 + i * 2) / 65535.0f;
        return true;
    }
    if (type == Sig("para")) {
        uint16_t fn = ReadU16(data + 8);
        static const int counts[] = {1, 3, 4, 5, 7};
        if (fn > 4) return false;
        int n = counts[fn];
        if (size < 12 + (size_t)n * 4) return false;
        curve.kind = ToneCurve::Kind::Parametric;
        curve.parametricType = fn;
        for (int i = 0; i < n; ++i) curve.params[i] = ReadS15Fixed16(data + 12 + i * 4);
        return true;
    }
    return false;
}

double ApplyPositive(const ToneCurve& c, double v) {
    switch (c.kind) {
    case ToneCurve::Kind::Identity:
        return v;
    case ToneCurve::Kind::Gamma:
        return std::pow(v, c.gamma);
    case ToneCurve::Kind::Table: {
        if (c.table.size() < 2) return v;
        double pos = std::clamp(v, 0.0, 1.0) * (double)(c.table.size() - 1);
        size_t i = (size_t)pos;
        if (i + 1 >= c.table.size()) return c.table.back();
        double f = pos - (double)i;
        return c.table[i] * (1 - f) + c.table[i + 1] * f;
    }
    case ToneCurve::Kind::Parametric: {
        const double* p = c.params;
        double g = p[0];
        switch (c.parametricType) {
        case 0: return std::pow(v, g);
        case 1: return v >= -p[2] / p[1] ? std::pow(p[1] * v + p[2], g) : 0.0;
        case 2: return v >= -p[2] / p[1] ? std::pow(p[1] * v + p[2], g) + p[3] : p[3];
        case 3: return v >= p[4] ? std::pow(p[1] * v + p[2], g) : p[3] * v;
        case 4: return v >= p[4] ? std::pow(p[1] * v + p[2], g) + p[5] : p[3] * v + p[6];
        default: return v;
        }
    }
    }
    return v;
}

}  // namespace

// Odd symmetry outside 0..1. An extended-range encoding stores a colour outside the
// container gamut as a negative number; mirroring keeps that sign and magnitude instead
// of clamping it to black, which is the whole point of measuring these files.
double ToneCurve::ToLinear(double v) const {
    if (v < 0) return -ApplyPositive(*this, -v);
    return ApplyPositive(*this, v);
}

bool ParseMatrixShaper(const uint8_t* data, size_t size, MatrixShaper& out) {
    if (size < 132) return false;
    if (ReadU32(data + 36) != Sig("acsp")) return false;
    if (ReadU32(data + 16) != Sig("RGB ")) return false;
    if (ReadU32(data + 20) != Sig("XYZ ")) return false;
    uint32_t tagCount = ReadU32(data + 128);
    if (tagCount > 1024 || 132 + (size_t)tagCount * 12 > size) return false;

    bool failed = false;
    auto findTag = [&](const char* sig, const uint8_t** ptr, uint32_t* len) -> bool {
        for (uint32_t i = 0; i < tagCount; ++i) {
            const uint8_t* e = data + 132 + (size_t)i * 12;
            if (ReadU32(e) != Sig(sig)) continue;
            uint32_t off = ReadU32(e + 4), sz = ReadU32(e + 8);
            if ((size_t)off + sz > size) { failed = true; return false; }
            *ptr = data + off; *len = sz;
            return true;
        }
        return false;
    };

    const char* colorantTags[3] = {"rXYZ", "gXYZ", "bXYZ"};
    double colorants[3][3];
    for (int c = 0; c < 3; ++c) {
        const uint8_t* p = nullptr; uint32_t len = 0;
        if (!findTag(colorantTags[c], &p, &len) || len < 20) return false;
        if (ReadU32(p) != Sig("XYZ ")) return false;
        for (int r = 0; r < 3; ++r) colorants[c][r] = ReadS15Fixed16(p + 8 + r * 4);
    }
    // Colorant tags are the columns of the RGB-to-XYZ matrix.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out.toXYZD50[r * 3 + c] = colorants[c][r];

    const char* trcTags[3] = {"rTRC", "gTRC", "bTRC"};
    for (int c = 0; c < 3; ++c) {
        const uint8_t* p = nullptr; uint32_t len = 0;
        if (!findTag(trcTags[c], &p, &len)) return false;
        if (!ParseCurve(p, len, out.trc[c])) return false;
    }
    if (failed) return false;

    const uint8_t* d = nullptr; uint32_t dlen = 0;
    if (findTag("desc", &d, &dlen) && dlen > 12) {
        if (ReadU32(d) == Sig("desc")) {
            uint32_t ascii = ReadU32(d + 8);
            if (ascii > 0 && 12 + (size_t)ascii <= dlen)
                out.description.assign((const char*)d + 12, ascii - 1);
        } else if (ReadU32(d) == Sig("mluc") && dlen >= 28) {
            uint32_t len = ReadU32(d + 20), off = ReadU32(d + 24);
            if ((size_t)off + len <= dlen) {
                std::wstring w;
                for (uint32_t i = 0; i + 1 < len; i += 2) w.push_back((wchar_t)ReadU16(d + off + i));
                out.description = Narrow(w);
            }
        }
    }
    return true;
}

Matrix3 Multiply(const Matrix3& a, const Matrix3& b);
Matrix3 Invert(const Matrix3& m);

namespace {
Matrix3 RgbToXYZ(double rx, double ry, double gx, double gy,
                 double bx, double by, double wx, double wy) {
    Matrix3 p{rx / ry, gx / gy, bx / by,
              1.0, 1.0, 1.0,
              (1 - rx - ry) / ry, (1 - gx - gy) / gy, (1 - bx - by) / by};
    Matrix3 pinv = Invert(p);
    double w[3] = {wx / wy, 1.0, (1 - wx - wy) / wy};
    double s[3];
    for (int r = 0; r < 3; ++r) s[r] = pinv[r * 3] * w[0] + pinv[r * 3 + 1] * w[1] + pinv[r * 3 + 2] * w[2];
    Matrix3 m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r * 3 + c] = p[r * 3 + c] * s[c];
    return m;
}
}  // namespace

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    return m;
}

Matrix3 Invert(const Matrix3& m) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7])
               - m[1] * (m[3] * m[8] - m[5] * m[6])
               + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(det) < 1e-12) throw ScopeError("色空間の変換行列が特異です。");
    double inv = 1.0 / det;
    Matrix3 o{};
    o[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
    o[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
    o[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
    o[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
    o[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
    o[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
    o[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
    o[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
    o[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
    return o;
}


Matrix3 PrimariesToLinearSRGB(double rx, double ry, double gx, double gy,
                              double bx, double by, double wx, double wy) {
    // Rec.709 primaries and D65: the space every measurement in this tool is in.
    Matrix3 srgb = RgbToXYZ(0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290);
    Matrix3 src = RgbToXYZ(rx, ry, gx, gy, bx, by, wx, wy);
    return Multiply(Invert(srgb), src);
}

// Bradford chromatic adaptation between two white points.
static Matrix3 BradfordAdapt(double sx, double sy, double dx, double dy) {
    const Matrix3 cone{ 0.8951,  0.2664, -0.1614,
                       -0.7502,  1.7135,  0.0367,
                        0.0389, -0.0685,  1.0296};
    Matrix3 coneInv = Invert(cone);
    auto whiteXYZ = [](double x, double y) {
        return std::array<double, 3>{x / y, 1.0, (1 - x - y) / y};
    };
    auto toCone = [&](std::array<double, 3> w) {
        std::array<double, 3> c{};
        for (int r = 0; r < 3; ++r) c[r] = cone[r * 3] * w[0] + cone[r * 3 + 1] * w[1] + cone[r * 3 + 2] * w[2];
        return c;
    };
    auto src = toCone(whiteXYZ(sx, sy));
    auto dst = toCone(whiteXYZ(dx, dy));
    Matrix3 scale{dst[0] / src[0], 0, 0, 0, dst[1] / src[1], 0, 0, 0, dst[2] / src[2]};
    return Multiply(coneInv, Multiply(scale, cone));
}

// Linear sRGB to the ICC profile connection space. Derived rather than copied from a
// published table so that the colorants written into a profile and the matrix used to
// read one back are the same numbers, and a file this tool wrote comes back unchanged
// apart from the s15Fixed16 quantization the ICC container forces.
const Matrix3& LinearSRGBToXYZD50() {
    static const Matrix3 m = [] {
        Matrix3 toD65 = RgbToXYZ(0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290);
        // ICC fixes the PCS white at D50 = (0.9642, 1.0, 0.8249) in XYZ, whose
        // chromaticity is the pair below.
        return Multiply(BradfordAdapt(0.3127, 0.3290, 0.345702914, 0.358538481), toD65);
    }();
    return m;
}

const Matrix3& XYZD50ToLinearSRGB() {
    static const Matrix3 m = Invert(LinearSRGBToXYZD50());
    return m;
}

std::string LinearSRGBDescription() {
    return "HDRScope linear sRGB (Rec.709 primaries, D65, gamma 1.0)";
}

std::string DisplayCopyDescription(double referenceWhiteNits) {
    return Format("HDRScope display copy (Rec.709 primaries, D65, gamma 1.0, 1.0 = %.0f nit)",
                  referenceWhiteNits);
}

std::vector<uint8_t> BuildLinearSRGBProfile(const std::string& description) {
    struct Tag { const char* sig; std::vector<uint8_t> body; };
    auto xyzTag = [](double x, double y, double z) {
        std::vector<uint8_t> v;
        WriteTag(v, "XYZ "); WriteU32(v, 0);
        WriteS15Fixed16(v, x); WriteS15Fixed16(v, y); WriteS15Fixed16(v, z);
        return v;
    };
    // A curveType with a zero-length curve is the ICC spelling of gamma 1.0.
    std::vector<uint8_t> identityCurve;
    WriteTag(identityCurve, "curv"); WriteU32(identityCurve, 0); WriteU32(identityCurve, 0);

    const char* text = description.c_str();
    std::vector<uint8_t> desc;
    WriteTag(desc, "desc"); WriteU32(desc, 0);
    uint32_t asciiLen = (uint32_t)description.size() + 1;
    WriteU32(desc, asciiLen);
    desc.insert(desc.end(), text, text + asciiLen);
    WriteU32(desc, 0);                      // unicode language code
    WriteU32(desc, 0);                      // unicode count
    desc.push_back(0); desc.push_back(0);   // script code
    desc.push_back(0);                      // mac description length
    desc.insert(desc.end(), 67, 0);

    const char* copy = "Public domain";
    std::vector<uint8_t> cprt;
    WriteTag(cprt, "text"); WriteU32(cprt, 0);
    cprt.insert(cprt.end(), copy, copy + strlen(copy) + 1);

    // The colorant tags are the columns of the linear sRGB to XYZ(D50) matrix.
    const Matrix3& toPCS = LinearSRGBToXYZD50();
    std::vector<Tag> tags = {
        {"desc", desc},
        {"wtpt", xyzTag(0.96420, 1.00000, 0.82491)},
        {"rXYZ", xyzTag(toPCS[0], toPCS[3], toPCS[6])},
        {"gXYZ", xyzTag(toPCS[1], toPCS[4], toPCS[7])},
        {"bXYZ", xyzTag(toPCS[2], toPCS[5], toPCS[8])},
        {"rTRC", identityCurve},
        {"gTRC", identityCurve},
        {"bTRC", identityCurve},
        {"cprt", cprt},
    };

    std::vector<uint8_t> body;
    struct Placed { uint32_t offset, size; };
    std::vector<Placed> placed;
    uint32_t base = 128 + 4 + (uint32_t)tags.size() * 12;
    for (auto& t : tags) {
        while (body.size() % 4) body.push_back(0);
        placed.push_back({base + (uint32_t)body.size(), (uint32_t)t.body.size()});
        body.insert(body.end(), t.body.begin(), t.body.end());
    }

    std::vector<uint8_t> icc;
    icc.reserve(base + body.size());
    WriteU32(icc, base + (uint32_t)body.size());   // profile size
    WriteU32(icc, 0);                              // preferred CMM
    WriteU32(icc, 0x02100000);                     // version 2.1
    WriteTag(icc, "mntr"); WriteTag(icc, "RGB "); WriteTag(icc, "XYZ ");
    icc.insert(icc.end(), 12, 0);                  // creation date and time
    WriteTag(icc, "acsp");
    WriteTag(icc, "MSFT");                         // primary platform
    WriteU32(icc, 0);                              // flags
    WriteU32(icc, 0);                              // device manufacturer
    WriteU32(icc, 0);                              // device model
    WriteU32(icc, 0); WriteU32(icc, 0);            // device attributes
    WriteU32(icc, 1);                              // rendering intent: relative colorimetric
    WriteS15Fixed16(icc, 0.96420); WriteS15Fixed16(icc, 1.00000); WriteS15Fixed16(icc, 0.82491);
    WriteU32(icc, 0);                              // profile creator
    icc.insert(icc.end(), 16, 0);                  // profile ID
    icc.insert(icc.end(), 28, 0);                  // reserved
    WriteU32(icc, (uint32_t)tags.size());
    for (size_t i = 0; i < tags.size(); ++i) {
        WriteTag(icc, tags[i].sig);
        WriteU32(icc, placed[i].offset);
        WriteU32(icc, placed[i].size);
    }
    icc.insert(icc.end(), body.begin(), body.end());
    return icc;
}
