// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_index_converter.h"

using namespace D3D12::IndexConversion;
using Indices = std::vector<std::uint32_t>;

// Reference assembles primitives from a vertex history, independent of the
// converter's random-access swizzles and segment-length counting.
Indices Reference(Topology topology, const Indices& source, Restart restart) {
    Indices result, history;
    auto emit = [&](std::initializer_list<std::uint32_t> primitive) {
        result.insert(result.end(), primitive);
    };
    auto finish = [&] {
        if (topology == Topology::LineLoop && history.size() >= 2)
            emit({history.back(), history.front()});
        history.clear();
    };
    for (const auto v : source) {
        if (restart.enabled && v == restart.index) {
            finish();
            continue;
        }
        history.push_back(v);
        const auto n = history.size();
        switch (topology) {
        case Topology::Points:
            emit({v});
            break;
        case Topology::Lines:
            if (n % 2 == 0)
                emit({history[n - 2], v});
            break;
        case Topology::LineStrip:
        case Topology::LineLoop:
            if (n >= 2)
                emit({history[n - 2], v});
            break;
        case Topology::Triangles:
            if (n % 3 == 0)
                emit({history[n - 3], history[n - 2], v});
            break;
        case Topology::TriangleStrip:
            if (n >= 3) {
                auto a = history[n - 3], b = history[n - 2];
                if (n % 2 == 0)
                    std::swap(a, b);
                emit({a, b, v});
            }
            break;
        case Topology::TriangleFan:
        case Topology::Polygon:
            if (n >= 3)
                emit({history.front(), history[n - 2], v});
            break;
        case Topology::Quads:
            if (n % 4 == 0) {
                const auto a = history[n - 4], b = history[n - 3], c = history[n - 2];
                emit({a, b, c});
                emit({a, c, v});
            }
            break;
        case Topology::QuadStrip:
            if (n >= 4 && n % 2 == 0) {
                const auto a = history[n - 4], b = history[n - 3], c = history[n - 2];
                emit({a, v, b});
                emit({a, c, v});
            }
            break;
        }
    }
    finish();
    return result;
}
std::size_t Width(Format format) {
    return std::size_t{1} << static_cast<unsigned>(format);
}
std::vector<std::byte> Pack(const Indices& indices, Format format) {
    std::vector<std::byte> bytes;
    for (auto v : indices)
        for (std::size_t b = 0; b < Width(format); ++b)
            bytes.push_back(std::byte((v >> (b * 8)) & 255));
    return bytes;
}
template <typename E, typename F>
void Reject(F operation) {
    bool rejected{};
    try {
        operation();
    } catch (const E&) {
        rejected = true;
    }
    assert(rejected);
}
void Check(Topology topology, Format format, const Indices& source, Restart restart) {
    const auto packed = Pack(source, format);
    // Unaligned input and output exercise the actual packed-byte API.
    std::vector<std::byte> input{std::byte{0xcc}};
    input.insert(input.end(), packed.begin(), packed.end());
    const auto in = std::span<const std::byte>(input).subspan(1);
    const auto layout = Analyze(topology, format, in, restart);
    const auto expected = Reference(topology, source, restart);
    assert(layout.count == expected.size());
    assert(layout.format == (format == Format::Uint8 ? Format::Uint16 : format));
    const auto list = static_cast<unsigned>(topology) == 0   ? Topology::Points
                      : static_cast<unsigned>(topology) <= 3 ? Topology::Lines
                                                             : Topology::Triangles;
    assert(layout.topology == list);
    const auto expected_bytes = Pack(expected, layout.format);
    assert(layout.bytes == expected_bytes.size());
    std::vector<std::byte> output(layout.bytes + 2, std::byte{0xcd});
    const auto out = std::span(output).subspan(1, layout.bytes);
    const auto converted = Convert(topology, format, in, out, restart);
    assert(converted.count == layout.count && converted.bytes == layout.bytes);
    assert(std::vector<std::byte>(out.begin(), out.end()) == expected_bytes);
    assert(output.front() == std::byte{0xcd} && output.back() == std::byte{0xcd});
    if (layout.bytes) {
        std::fill(output.begin(), output.end(), std::byte{0xcd});
        Reject<std::out_of_range>(
            [&] { Convert(topology, format, in, out.first(out.size() - 1), restart); });
        for (auto b : output)
            assert(b == std::byte{0xcd});
    }
}
int main() {
    for (unsigned t = 0; t < 10; ++t) {
        const auto topology = static_cast<Topology>(t);
        for (auto format : {Format::Uint8, Format::Uint16, Format::Uint32}) {
            const auto max = format == Format::Uint8    ? 255u
                             : format == Format::Uint16 ? 65535u
                                                        : UINT32_MAX;
            // Exhaust every restart placement in sequences of lengths 0..10.
            // Includes leading/trailing/repeated markers, every short segment,
            // unfinished groups, and strip parity reset at all positions.
            for (unsigned n = 0; n <= 10; ++n) {
                for (unsigned pattern = 0; pattern < (1u << n); ++pattern) {
                    Indices source;
                    for (unsigned i = 0; i < n; ++i)
                        source.push_back(pattern & (1u << i) ? max : i + 7);
                    Check(topology, format, source, {true, max});
                }
            }
            Check(topology, format, {max, max, 0, max, 1, 1, 2, 3}, {});
            Check(topology, format, {0, 1, 2, 0, 4, 5, 6, 7, 0}, {true, 0});
            Check(topology, format, {1, 2, 3, 4, 5, 6, 7}, {true, 3});
            // Full-width restart register comparison: high bits are not truncated.
            if (format != Format::Uint32)
                Check(topology, format, {max, 1, 2, max, 3, 4}, {true, UINT32_MAX});
            std::mt19937 rng(812 + t);
            for (unsigned n = 0; n < 128; ++n) {
                Indices source;
                for (unsigned i = 0; i < n; ++i)
                    source.push_back(rng() % 5 ? rng() & max : max);
                Check(topology, format, source, {true, max});
                Check(topology, format, source, {});
            }
        }
        // All generated lengths and format boundaries, with nonzero first vertex.
        for (auto first : {0u, 65530u, 65535u, 65536u, UINT32_MAX - 128}) {
            for (unsigned n = 0; n < 128; ++n) {
                Indices source;
                for (unsigned i = 0; i < n; ++i)
                    source.push_back(first + i);
                const auto expected = Reference(topology, source, {});
                const auto layout = AnalyzeGenerated(topology, n, first);
                assert(layout.count == expected.size());
                const auto format =
                    !n || std::uint64_t(first) + n - 1 <= 65535 ? Format::Uint16 : Format::Uint32;
                assert(layout.format == format);
                std::vector<std::byte> output(layout.bytes + 2, std::byte{0xde});
                const auto out = std::span(output).subspan(1, layout.bytes);
                Generate(topology, n, out, first);
                assert(std::vector<std::byte>(out.begin(), out.end()) == Pack(expected, format));
                assert(output.front() == std::byte{0xde} && output.back() == std::byte{0xde});
                if (layout.bytes)
                    Reject<std::out_of_range>(
                        [&] { Generate(topology, n, out.first(layout.bytes - 1), first); });
            }
        }
    }
    // Explicit winding and indexed quad-strip mapping, including duplicated indices.
    assert((Reference(Topology::QuadStrip, {10, 20, 30, 40, 50, 60}, {}) ==
            Indices{10, 40, 20, 10, 30, 40, 30, 60, 40, 30, 50, 60}));
    std::array<std::byte, 32> buffer{};
    Reject<std::invalid_argument>([&] { Analyze(static_cast<Topology>(99), Format::Uint8, {}); });
    Reject<std::invalid_argument>([&] { Analyze(Topology::Points, static_cast<Format>(99), {}); });
    Reject<std::invalid_argument>(
        [&] { Analyze(Topology::Points, Format::Uint16, std::span(buffer).first(1)); });
    Reject<std::invalid_argument>(
        [&] { Analyze(Topology::Points, Format::Uint32, std::span(buffer).first(3)); });
    Reject<std::invalid_argument>(
        [&] { Convert(Topology::Points, Format::Uint8, std::span(buffer).first(4), buffer); });
    Reject<std::invalid_argument>(
        [&] { Convert(Topology::Points, Format::Uint8, std::span(buffer).subspan(1, 4), buffer); });
    Reject<std::overflow_error>([&] { AnalyzeGenerated(Topology::Points, 2, UINT32_MAX); });
    Reject<std::overflow_error>([&] { AnalyzeGenerated(Topology::TriangleFan, UINT32_MAX); });
    Reject<std::overflow_error>([&] { AnalyzeGenerated(Topology::LineLoop, UINT32_MAX); });
    Reject<std::overflow_error>([&] { AnalyzeGenerated(Topology::QuadStrip, UINT32_MAX); });
    Reject<std::overflow_error>([&] { AnalyzeGenerated(Topology::Quads, UINT32_MAX); });
    assert(AnalyzeGenerated(Topology::Points, UINT32_MAX).count == UINT32_MAX);
    assert(AnalyzeGenerated(Topology::TriangleStrip, 1431655767).count == UINT32_MAX);
    Reject<std::overflow_error>([&] { AnalyzeGenerated(Topology::TriangleStrip, 1431655768); });
    Generate(Topology::Points, 0, {}, UINT32_MAX);
    Check(Topology::Points, Format::Uint8, {255}, {}); // 255 stays an index without restart.
}
