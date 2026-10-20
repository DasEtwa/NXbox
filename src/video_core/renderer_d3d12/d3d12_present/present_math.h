// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace D3D12::Present {
struct Extent {
    std::uint32_t width{}, height{};
    bool operator==(const Extent&) const = default;
};
struct Crop {
    float left{}, top{}, right{1}, bottom{1};
};
inline void ValidateExtent(Extent size, bool output = false) {
    if (!size.width || !size.height || (output && (size.width > 3840 || size.height > 2160)))
        throw std::invalid_argument("Invalid presentation extent");
}
// Normalize in guest coordinates, not scaled texture coordinates. Thus a 2x
// cache image and a raw upload select the same visible guest rectangle.
inline Crop NormalizeCrop(Crop pixels, Extent guest) {
    ValidateExtent(guest);
    const std::array values{pixels.left, pixels.top, pixels.right, pixels.bottom};
    for (float value : values)
        if (!std::isfinite(value))
            throw std::invalid_argument("Non-finite presentation crop");
    if (pixels.left < 0 || pixels.top < 0 || pixels.right > guest.width ||
        pixels.bottom > guest.height || pixels.right <= pixels.left || pixels.bottom <= pixels.top)
        throw std::invalid_argument("Invalid presentation crop");
    return {pixels.left / guest.width, pixels.top / guest.height, pixels.right / guest.width,
            pixels.bottom / guest.height};
}
inline std::array<float, 2> TransformUv(std::array<float, 2> uv, std::uint32_t flags) {
    if (flags & 4)
        uv = {uv[1], 1 - uv[0]};
    if (flags & 1)
        uv[0] = 1 - uv[0];
    if (flags & 2)
        uv[1] = 1 - uv[1];
    return uv;
}
inline unsigned SyncInterval(bool vsync, unsigned interval = 1) {
    if (interval < 1 || interval > 4)
        throw std::invalid_argument("DXGI vsync interval must be between 1 and 4");
    return vsync ? interval : 0;
}
// Compare the visible source rectangle in source axes, before final rotation.
inline bool NeedsUpscale(Extent source, Extent output, Crop crop, std::uint32_t transform) {
    const Extent destination = (transform & 4) ? Extent{output.height, output.width} : output;
    return (crop.right - crop.left) * source.width < destination.width ||
           (crop.bottom - crop.top) * source.height < destination.height;
}
inline std::array<float, 4> EasuMapping(Extent source, Extent output, Crop crop) {
    ValidateExtent(source);
    ValidateExtent(output);
    const float x = (crop.right - crop.left) * source.width / output.width;
    const float y = (crop.bottom - crop.top) * source.height / output.height;
    return {x, y, .5f * x - .5f + crop.left * source.width,
            .5f * y - .5f + crop.top * source.height};
}
inline float RcasStrength(float sharpness) {
    if (!std::isfinite(sharpness))
        throw std::invalid_argument("Non-finite FSR sharpness");
    return std::exp2(-std::clamp(sharpness, 0.f, 1.f));
}
inline std::array<float, 4> CubicWeights(float x) {
    const float x2 = x * x, x3 = x2 * x;
    return {(-x + 2 * x2 - x3) * .5f, (2 - 5 * x2 + 3 * x3) * .5f, (x + 4 * x2 - 3 * x3) * .5f,
            (-x2 + x3) * .5f};
}
inline float AreaOverlap(float begin, float end, int texel) {
    return std::max(0.f, std::min(end, float(texel + 1)) - std::max(begin, float(texel)));
}
} // namespace D3D12::Present
