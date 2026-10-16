// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Host-testable artwork math. Pixels are premultiplied BGRA, colors are sRGB.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace EdenXbox::Ui::Art {
struct Color {
    float r = 0, g = 0, b = 0;
};
using Palette = std::array<Color, 3>;
inline constexpr float kLeftScrimOpacity = .82f;
inline constexpr float kLeftScrimEnd = .64f;
inline constexpr float kBottomScrimStart = .82f;
inline constexpr std::array<float, 3> kGradientStops{0.0f, 0.48f, 0.72f};
inline Color Mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
inline float Luminance(Color c) {
    const auto linear = [](float v) {
        return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
    };
    return .2126f * linear(c.r) + .7152f * linear(c.g) + .0722f * linear(c.b);
}
inline Color Scale(Color c, float scale) {
    return {c.r * scale, c.g * scale, c.b * scale};
}
inline Color Gradient(const Palette& palette, float y) {
    if (y <= kGradientStops[1])
        return Mix(palette[0], palette[1], std::clamp(y / kGradientStops[1], 0.0f, 1.0f));
    return Mix(
        palette[1], palette[2],
        std::clamp((y - kGradientStops[1]) / (kGradientStops[2] - kGradientStops[1]), 0.0f, 1.0f));
}
inline Palette Seeded(std::string_view id) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : id)
        hash = (hash ^ c) * 16777619u;
    Color c{.18f + (hash & 255) / 510.0f, .18f + ((hash >> 8) & 255) / 510.0f,
            .18f + ((hash >> 16) & 255) / 510.0f};
    return {c, Scale(c, .65f), Scale(c, .28f)};
}

// Area-average downsampling avoids aliasing tiny details into the dominant colors.
inline std::vector<Color> Downsample(const std::vector<std::uint8_t>& bgra, unsigned width,
                                     unsigned height, unsigned out_width, unsigned out_height) {
    if (!width || !height || !out_width || !out_height || width > 4096 || height > 4096 ||
        bgra.size() != std::size_t(width) * height * 4)
        return {};
    std::vector<Color> result(out_width * out_height);
    for (unsigned y = 0; y < out_height; ++y) {
        for (unsigned x = 0; x < out_width; ++x) {
            Color sum;
            float weight = 0;
            for (unsigned sy = y * height / out_height;
                 sy < std::max(y * height / out_height + 1, (y + 1) * height / out_height); ++sy) {
                for (unsigned sx = x * width / out_width;
                     sx < std::max(x * width / out_width + 1, (x + 1) * width / out_width); ++sx) {
                    const auto i = (std::size_t(sy) * width + sx) * 4;
                    sum.r += bgra[i + 2] / 255.0f;
                    sum.g += bgra[i + 1] / 255.0f;
                    sum.b += bgra[i] / 255.0f;
                    weight += bgra[i + 3] / 255.0f;
                }
            }
            result[y * out_width + x] = weight > 0 ? Scale(sum, 1.0f / weight) : Color{};
        }
    }
    return result;
}

// Deterministic k-means on 32x32 samples. Farthest-point seeds keep a small vivid region;
// sorting by luminance makes the gradient flow towards the dark lower edge.
inline Palette Dominant(const std::vector<std::uint8_t>& bgra, unsigned width, unsigned height) {
    const auto samples = Downsample(bgra, width, height, 32, 32);
    if (samples.empty())
        return Seeded("");
    const auto distance = [](Color a, Color b) {
        return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
    };
    Palette centers{samples[0], samples[0], samples[0]};
    for (unsigned k = 1; k < 3; ++k) {
        float farthest = -1;
        for (auto c : samples) {
            float nearest = 4;
            for (unsigned j = 0; j < k; ++j)
                nearest = std::min(nearest, distance(c, centers[j]));
            if (nearest > farthest) {
                farthest = nearest;
                centers[k] = c;
            }
        }
    }
    for (unsigned iteration = 0; iteration < 10; ++iteration) {
        Palette sums{};
        std::array<unsigned, 3> counts{};
        for (auto c : samples) {
            unsigned best = 0;
            for (unsigned k = 1; k < 3; ++k)
                if (distance(c, centers[k]) < distance(c, centers[best]))
                    best = k;
            sums[best].r += c.r;
            sums[best].g += c.g;
            sums[best].b += c.b;
            ++counts[best];
        }
        for (unsigned k = 0; k < 3; ++k)
            if (counts[k])
                centers[k] = Scale(sums[k], 1.0f / counts[k]);
    }
    std::sort(centers.begin(), centers.end(),
              [](Color a, Color b) { return Luminance(a) > Luminance(b); });
    if (Luminance(centers[0]) - Luminance(centers[2]) < .12f) {
        // Black/near-black artwork needs a small lift to leave room for distinct shades.
        while (Luminance(centers[0]) < .16f)
            centers[0] = Mix(centers[0], Color{1, 1, 1}, .05f);
        centers[1] = Scale(centers[0], .65f);
        centers[2] = Scale(centers[0], .28f);
    }
    return centers;
}

// Three separable box passes approximate a Gaussian, at only 64x64. The square is then
// center-cropped to cover the wide hero. The cached BMP contains image + color, never text.
inline std::vector<std::uint8_t> Hero(const std::vector<std::uint8_t>& bgra, unsigned width,
                                      unsigned height) {
    auto thumb = Downsample(bgra, width, height, 64, 64);
    if (thumb.empty())
        return {};
    const auto palette = Dominant(bgra, width, height);
    std::vector<Color> work(thumb.size());
    constexpr int radius = 9;
    for (int pass = 0; pass < 6; ++pass) {
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                Color sum;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const auto c = thumb[std::clamp(y + (pass % 2 ? offset : 0), 0, 63) * 64 +
                                         std::clamp(x + (pass % 2 ? 0 : offset), 0, 63)];
                    sum.r += c.r;
                    sum.g += c.g;
                    sum.b += c.b;
                }
                work[y * 64 + x] = Scale(sum, 1.0f / (2 * radius + 1));
            }
        }
        thumb.swap(work);
    }
    std::vector<std::uint8_t> result(192 * 76 * 4);
    for (unsigned y = 0; y < 76; ++y) {
        for (unsigned x = 0; x < 192; ++x) {
            auto c = thumb[(19 + y * 25 / 76) * 64 + x * 64 / 192];
            const float gray = .2126f * c.r + .7152f * c.g + .0722f * c.b;
            c = {std::clamp(gray + (c.r - gray) * 1.18f, 0.0f, 1.0f),
                 std::clamp(gray + (c.g - gray) * 1.18f, 0.0f, 1.0f),
                 std::clamp(gray + (c.b - gray) * 1.18f, 0.0f, 1.0f)};
            c = Mix(c, Gradient(palette, y / 75.0f), .42f);
            const auto i = (y * 192 + x) * 4;
            result[i] = static_cast<std::uint8_t>(c.b * 255);
            result[i + 1] = static_cast<std::uint8_t>(c.g * 255);
            result[i + 2] = static_cast<std::uint8_t>(c.r * 255);
            result[i + 3] = 255;
        }
    }
    return result;
}
} // namespace EdenXbox::Ui::Art
