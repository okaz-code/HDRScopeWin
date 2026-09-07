#include "PixelImage.h"
#include <algorithm>
#include <atomic>
#include <cmath>

RectD RectD::Intersect(const RectD& other) const {
    double x0 = std::max(MinX(), other.MinX());
    double y0 = std::max(MinY(), other.MinY());
    double x1 = std::min(MaxX(), other.MaxX());
    double y1 = std::min(MaxY(), other.MaxY());
    if (x1 <= x0 || y1 <= y0) return RectD{0, 0, 0, 0};
    return RectD{x0, y0, x1 - x0, y1 - y0};
}

std::string ContentSummary::HdrText() const {
    return overOne > 0
        ? Format("最大RGB %.4f · 1.0超 %.3f%%", maxRGB, overOne * 100)
        : Format("最大RGB %.4f · 1.0超なし（SDR範囲）", maxRGB);
}

std::string Sample::Text() const {
    return Format("R: %.6f   G: %.6f   B: %.6f   A: %.4f", r, g, b, a);
}

std::string Sample::DetailText() const {
    return Format("拡張リニアsRGB · 1.0 = SDR白 · x %.3f–%.3f / y %.3f–%.3f · %d画素%s",
                  rect.MinX(), rect.MaxX(), rect.MinY(), rect.MaxY(),
                  count, count > 1 ? "の面積加重平均" : "");
}

static std::atomic<uint64_t> g_nextImageId{1};

PixelImage::PixelImage(int width, int height, std::vector<float> pixels)
    : width_(width), height_(height), pixels_(std::move(pixels)), id_(g_nextImageId++) {
    if (width <= 0 || height <= 0 || pixels_.size() != (size_t)width * height * 4)
        throw ScopeError("画素バッファの大きさが画像の寸法と一致しません。");
}

ContentSummary PixelImage::Summary() const {
    ContentSummary s;
    s.maxAlpha = 0;
    s.maxRGB = -std::numeric_limits<float>::max();
    s.minRGB = std::numeric_limits<float>::max();
    s.uniform = true;
    size_t over = 0;
    for (size_t i = 0; i < pixels_.size(); i += 4) {
        s.maxAlpha = std::max(s.maxAlpha, pixels_[i + 3]);
        for (int c = 0; c < 3; ++c) {
            float v = pixels_[i + c];
            s.maxRGB = std::max(s.maxRGB, v);
            s.minRGB = std::min(s.minRGB, v);
            if (v > 1) ++over;
        }
        if (s.uniform && i > 0) {
            s.uniform = pixels_[i] == pixels_[0] && pixels_[i + 1] == pixels_[1]
                     && pixels_[i + 2] == pixels_[2] && pixels_[i + 3] == pixels_[3];
        }
    }
    s.overOne = (double)over / ((double)width_ * height_ * 3);
    return s;
}

// Area-weighted mean in double precision over the requested rectangle in image
// coordinates. Weights are the overlap of each source pixel with the rectangle, so a
// cell that falls partly outside the image is normalized by the area actually covered.
std::optional<Sample> PixelImage::SampleRect(const RectD& requested) const {
    RectD rect = requested.Intersect(RectD{0, 0, (double)width_, (double)height_});
    if (rect.Empty()) return std::nullopt;
    double sum[4] = {0, 0, 0, 0};
    double area = 0;
    int count = 0;
    int y0 = (int)std::floor(rect.MinY());
    int y1 = std::min(height_, (int)std::ceil(rect.MaxY()));
    int x0 = (int)std::floor(rect.MinX());
    int x1 = std::min(width_, (int)std::ceil(rect.MaxX()));
    for (int y = std::max(0, y0); y < y1; ++y) {
        double dy = std::max(0.0, std::min((double)(y + 1), rect.MaxY()) - std::max((double)y, rect.MinY()));
        if (dy <= 0) continue;
        for (int x = std::max(0, x0); x < x1; ++x) {
            double dx = std::max(0.0, std::min((double)(x + 1), rect.MaxX()) - std::max((double)x, rect.MinX()));
            double weight = dx * dy;
            if (weight <= 0) continue;
            size_t i = ((size_t)y * width_ + x) * 4;
            for (int c = 0; c < 4; ++c) sum[c] += (double)pixels_[i + c] * weight;
            area += weight;
            ++count;
        }
    }
    if (area <= 0) return std::nullopt;
    Sample s;
    s.rect = rect;
    s.r = sum[0] / area; s.g = sum[1] / area; s.b = sum[2] / area; s.a = sum[3] / area;
    s.count = count;
    return s;
}

const std::vector<float>& PixelImage::TestPatternLevels() {
    static const std::vector<float> levels = [] {
        std::vector<float> v{0.0f, 0.09f, 0.18f, 0.5f};
        for (int i = 1; i <= 30; ++i) v.push_back((float)i);
        return v;
    }();
    return levels;
}

// The conversion table from the README, so negative handling stays visible in the pattern.
static const float kOutOfGamut[][3] = {
    {-0.25f, -0.25f, -0.25f}, {-0.1f, 1.5f, 0.2f},
    {1.2249f, -0.0421f, -0.0196f}, {-0.2249f, 1.0421f, -0.0786f},
    {1.6605f, -0.1246f, -0.0182f}, {-0.5876f, 1.1329f, -0.1006f},
};
static const float kTints[][3] = {{1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

// The fixed ladder covers every display but spends most of its steps above what any of
// them can show: on a 5.79x panel only six of its thirty-four columns are in range. This
// one puts every column where the answer is, by sizing the step to the headroom.
//
// The step is a power-of-two fraction rather than headroom/n, so SDR white lands exactly
// on a column and the values stay easy to say out loud. Four steps continue past the
// headroom, because a ladder that stops at the limit shows no plateau: seeing where the
// steps stop separating is the whole point, and that needs steps on the far side of it.
PixelImage::HeadroomLadder PixelImage::HeadroomLevels(double headroom) {
    HeadroomLadder ladder;
    ladder.headroom = std::max(1.0, headroom);
    // Every candidate divides 1.0 exactly, so SDR white always lands on a step and can
    // be used as the reference patch no matter what the headroom turns out to be.
    static const float kSteps[] = {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f};
    const int kMaxColumns = 34, kStepsPastHeadroom = 4;
    ladder.step = kSteps[std::size(kSteps) - 1];
    for (float step : kSteps) {
        if ((int)std::ceil(ladder.headroom / step) + kStepsPastHeadroom <= kMaxColumns) {
            ladder.step = step;
            break;
        }
    }
    int columns = std::min(kMaxColumns,
                           (int)std::ceil(ladder.headroom / ladder.step) + kStepsPastHeadroom);
    for (int i = 0; i < columns; ++i) ladder.levels.push_back(ladder.step * i);
    ladder.top = ladder.levels.back();
    // A headroom past 33x SDR white does not fit even at whole steps. The ladder then
    // stops short of it, which has to be said rather than left to look like a plateau.
    ladder.reachesHeadroom = ladder.top >= ladder.headroom;
    return ladder;
}

std::shared_ptr<PixelImage> PixelImage::TestPattern() { return Pattern(TestPatternLevels()); }

std::shared_ptr<PixelImage> PixelImage::Pattern(const std::vector<float>& levels) {
    const int cell = 40, rowHeight = 180, stripHeight = 60;
    const int tintCount = 4, gamutCount = (int)std::size(kOutOfGamut);
    if (levels.empty()) throw ScopeError("テストパターンの階調が空です。");
    int w = (int)levels.size() * cell;
    int ladderHeight = tintCount * rowHeight;
    int h = ladderHeight + stripHeight;
    std::vector<float> p((size_t)w * h * 4, 1.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float c[3];
            if (y < ladderHeight) {
                const float* tint = kTints[y / rowHeight];
                float level = levels[(size_t)(x / cell)];
                c[0] = tint[0] * level; c[1] = tint[1] * level; c[2] = tint[2] * level;
            } else {
                const float* g = kOutOfGamut[std::min(gamutCount - 1, x * gamutCount / w)];
                c[0] = g[0]; c[1] = g[1]; c[2] = g[2];
            }
            size_t i = ((size_t)y * w + x) * 4;
            p[i] = c[0]; p[i + 1] = c[1]; p[i + 2] = c[2];
        }
    }
    return std::make_shared<PixelImage>(w, h, std::move(p));
}
