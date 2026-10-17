// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace D3D12 {
// SDK-independent validation shared by stream output and host tests. Attributes
// are Maxwell scalar locations; a missing attribute represents a declaration gap.
struct StreamOutputVarying {
    std::uint32_t attribute, buffer, stream, stride, offset, components;
};
struct StreamOutputElement {
    static constexpr std::uint32_t Gap = UINT32_MAX;
    std::uint32_t attribute, stream, buffer, start_component, components;
};
struct StreamOutputLayout {
    std::vector<StreamOutputElement> elements;
    std::array<std::uint32_t, 4> strides{};
    std::array<std::uint32_t, 4> streams{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
};
inline StreamOutputLayout MakeStreamOutputLayout(std::span<const StreamOutputVarying> varyings) {
    StreamOutputLayout result;
    std::vector<StreamOutputVarying> sorted(varyings.begin(), varyings.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.buffer != b.buffer ? a.buffer < b.buffer : a.offset < b.offset;
    });
    std::array<std::uint32_t, 4> ends{}, component_counts{};
    for (const auto& v : sorted) {
        if (v.buffer >= 4 || v.stream >= 4 || !v.components || v.components > 4 ||
            v.attribute >= 256 || v.attribute % 4 + v.components > 4 || !v.stride ||
            v.stride > 2048 || (v.stride | v.offset) % 4 || v.offset > v.stride ||
            v.components * 4 > v.stride - v.offset) {
            throw std::invalid_argument("Invalid stream output varying");
        }
        auto& stride = result.strides[v.buffer];
        auto& stream = result.streams[v.buffer];
        if ((stride && stride != v.stride) || (stream != UINT32_MAX && stream != v.stream) ||
            v.offset < ends[v.buffer]) {
            throw std::invalid_argument("Conflicting stream output layout");
        }
        stride = v.stride;
        stream = v.stream;
        const auto gap = (v.offset - ends[v.buffer]) / 4;
        if (gap) {
            result.elements.push_back({StreamOutputElement::Gap, v.stream, v.buffer, 0, gap});
        }
        result.elements.push_back({v.attribute, v.stream, v.buffer, v.attribute % 4, v.components});
        ends[v.buffer] = v.offset + v.components * 4;
        component_counts[v.stream] += v.components;
        if (component_counts[v.stream] > 128) {
            throw std::invalid_argument("Stream output exceeds 128 components per stream");
        }
    }
    for (std::uint32_t buffer = 0; buffer < 4; ++buffer) {
        if (result.strides[buffer] > ends[buffer]) {
            result.elements.push_back({StreamOutputElement::Gap, result.streams[buffer], buffer, 0,
                                       (result.strides[buffer] - ends[buffer]) / 4});
        }
    }
    // Keep every output slot's entries together, including its trailing padding.
    std::stable_sort(result.elements.begin(), result.elements.end(),
                     [](const auto& a, const auto& b) { return a.buffer < b.buffer; });
    return result;
}
constexpr std::uint64_t CounterDifference(std::uint64_t value, std::uint64_t baseline) {
    return value >= baseline ? value - baseline : 0;
}
constexpr std::uint64_t StreamOutputOverflow(std::uint64_t needed, std::uint64_t written) {
    return CounterDifference(needed, written);
}
inline std::uint64_t TimestampNanoseconds(std::uint64_t ticks, std::uint64_t frequency) {
    if (!frequency) {
        throw std::invalid_argument("Zero timestamp frequency");
    }
    // Exact integer multiply/divide of the fractional ticks without a 128-bit
    // type (MSVC does not provide one). Each step keeps remainder < frequency.
    const auto fraction = ticks % frequency;
    std::uint64_t remainder{}, quotient{};
    for (int bit = 29; bit >= 0; --bit) {
        quotient *= 2;
        if (remainder >= frequency - remainder) {
            remainder -= frequency - remainder;
            ++quotient;
        } else {
            remainder *= 2;
        }
        if ((1000000000ULL >> bit) & 1) {
            if (remainder >= frequency - fraction) {
                remainder -= frequency - fraction;
                ++quotient;
            } else {
                remainder += fraction;
            }
        }
    }
    return (ticks / frequency) * 1000000000ULL + quotient;
}
} // namespace D3D12
