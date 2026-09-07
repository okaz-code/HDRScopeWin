#pragma once
#include "PixelImage.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Baseline TIFF with IEEE float samples, an embedded linear sRGB ICC profile and
// unassociated alpha. The float bytes are written directly, so nothing in the platform
// imaging stack gets a chance to colour convert or tone map on the way out.
namespace FloatTiff {

std::vector<uint8_t> Encode(const PixelImage& image);

// Writes atomically and then reads the real file back and compares it byte for byte.
void Save(const PixelImage& image, const std::wstring& path);

// Independent reader for exactly the layout Encode writes: uncompressed, one strip,
// RGBA float32, top-to-bottom. Used to verify a saved file without going through the
// platform decoder, and as the fallback when that decoder declines a float TIFF.
struct Decoded {
    int width = 0, height = 0;
    std::vector<float> pixels;
    bool hasIccProfile = false;
    std::string description;
};
std::optional<Decoded> Decode(const uint8_t* data, size_t size);

}  // namespace FloatTiff
