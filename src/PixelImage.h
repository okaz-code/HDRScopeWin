#pragma once
#include "Common.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    double MinX() const { return x; }
    double MinY() const { return y; }
    double MaxX() const { return x + w; }
    double MaxY() const { return y + h; }
    RectD Intersect(const RectD& other) const;
    bool Empty() const { return w <= 0 || h <= 0; }
};

// What the OS hands back for a window with nothing on screen is a fully transparent
// surface, not an error, so a capture has to be inspected to notice it. The same pass
// answers the question this tool exists for: is anything here above SDR white?
struct ContentSummary {
    float maxAlpha = 0;
    bool uniform = true;
    float maxRGB = 0;
    float minRGB = 0;
    double overOne = 0;
    std::string HdrText() const;
};

struct Sample {
    RectD rect;
    double r = 0, g = 0, b = 0, a = 0;
    int count = 0;
    std::string Text() const;
    std::string DetailText() const;
    // What a click puts on the clipboard: the same two lines shown under the image.
    std::string ClipboardText() const { return Text() + "\r\n" + DetailText(); }
};

// Top-to-bottom rows, straight alpha, extended linear sRGB (Rec.709 primaries, D65),
// 1.0 = SDR reference white. Values are never tone mapped or clipped.
class PixelImage {
public:
    PixelImage(int width, int height, std::vector<float> pixels);

    int Width() const { return width_; }
    int Height() const { return height_; }
    const std::vector<float>& Pixels() const { return pixels_; }
    uint64_t Id() const { return id_; }

    ContentSummary Summary() const;
    std::optional<Sample> SampleRect(const RectD& requested) const;

    // Four ladders - white, red, green, blue - over the given levels, with a strip of
    // out-of-gamut samples underneath. Where a ladder stops getting brighter is the
    // display's headroom, which is why the steps are flat patches: the last one you can
    // tell apart is the answer.
    static std::shared_ptr<PixelImage> Pattern(const std::vector<float>& levels);

    // The fixed ladder: 0 to 30x SDR white. Covers any display, at the cost of spending
    // most of its steps above what any of them can show.
    static const std::vector<float>& TestPatternLevels();
    static std::shared_ptr<PixelImage> TestPattern();

    // A ladder sized to one display: fine steps from black to a few steps past the
    // given headroom, so the point where the panel stops can be read off directly
    // instead of falling between two whole-number steps. The step is a power-of-two
    // fraction, so SDR white always lands exactly on a step.
    struct HeadroomLadder {
        std::vector<float> levels;
        float step = 1;
        float top = 1;
        double headroom = 1;
        bool reachesHeadroom = true;   // false when the headroom is past the last step
    };
    static HeadroomLadder HeadroomLevels(double headroom);

private:
    int width_, height_;
    std::vector<float> pixels_;
    uint64_t id_;
};

using PixelImagePtr = std::shared_ptr<PixelImage>;
