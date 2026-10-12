// SPDX-License-Identifier: GPL-3.0-or-later
#include <climits>
#include <limits>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_index_converter.h"

namespace D3D12::IndexConversion {
namespace {
std::size_t Width(Format format) {
    switch (format) {
    case Format::Uint8:
        return 1;
    case Format::Uint16:
        return 2;
    case Format::Uint32:
        return 4;
    }
    throw std::invalid_argument("Invalid index format");
}
Topology ListTopology(Topology topology) {
    switch (topology) {
    case Topology::Points:
        return Topology::Points;
    case Topology::Lines:
    case Topology::LineLoop:
    case Topology::LineStrip:
        return Topology::Lines;
    case Topology::Triangles:
    case Topology::TriangleStrip:
    case Topology::TriangleFan:
    case Topology::Quads:
    case Topology::QuadStrip:
    case Topology::Polygon:
        return Topology::Triangles;
    }
    throw std::invalid_argument("Unsupported index topology");
}
std::uint64_t SegmentCount(Topology topology, std::uint64_t n) {
    // Even discarded tails cannot make a longer segment fit a UINT draw count.
    // Bound before multiplying so adversarial spans cannot wrap the count.
    if (n > std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 2)
        throw std::overflow_error("Index segment exceeds D3D12 limits");
    switch (topology) {
    case Topology::Points:
        return n;
    case Topology::Lines:
        return n / 2 * 2;
    case Topology::LineStrip:
        return n >= 2 ? (n - 1) * 2 : 0;
    case Topology::LineLoop:
        return n >= 2 ? n * 2 : 0;
    case Topology::Triangles:
        return n / 3 * 3;
    case Topology::Quads:
        return n / 4 * 6;
    case Topology::QuadStrip:
        return n >= 4 ? (n / 2 - 1) * 6 : 0;
    case Topology::TriangleStrip:
    case Topology::TriangleFan:
    case Topology::Polygon:
        return n >= 3 ? (n - 2) * 3 : 0;
    }
    throw std::invalid_argument("Unsupported index topology");
}
Layout MakeLayout(Topology topology, Format format, std::uint64_t count) {
    const auto width = Width(format);
    if (count > std::numeric_limits<std::uint32_t>::max() ||
        count > std::numeric_limits<std::size_t>::max() / width)
        throw std::overflow_error("Converted index buffer is too large");
    return {ListTopology(topology), format, static_cast<std::uint32_t>(count),
            static_cast<std::size_t>(count) * width};
}
std::uint32_t Read(std::span<const std::byte> input, std::size_t width, std::size_t i) {
    std::uint32_t value{};
    for (std::size_t b = 0; b < width; ++b)
        value |= std::uint32_t(std::to_integer<unsigned char>(input[i * width + b])) << (8 * b);
    return value;
}
template <typename ReadIndex, typename Emit>
void EmitSegment(Topology topology, std::size_t begin, std::size_t n, ReadIndex read, Emit emit) {
    auto at = [&](std::size_t i) { return read(begin + i); };
    auto line = [&](std::size_t a, std::size_t b) {
        emit(at(a));
        emit(at(b));
    };
    auto triangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        emit(at(a));
        emit(at(b));
        emit(at(c));
    };
    switch (topology) {
    case Topology::Points:
        for (std::size_t i = 0; i < n; ++i)
            emit(at(i));
        break;
    case Topology::Lines:
        for (std::size_t i = 0; i + 1 < n; i += 2)
            line(i, i + 1);
        break;
    case Topology::LineStrip:
    case Topology::LineLoop:
        for (std::size_t i = 1; i < n; ++i)
            line(i - 1, i);
        if (topology == Topology::LineLoop && n >= 2)
            line(n - 1, 0);
        break;
    case Topology::Triangles:
        for (std::size_t i = 0; i + 2 < n; i += 3)
            triangle(i, i + 1, i + 2);
        break;
    case Topology::TriangleStrip:
        for (std::size_t i = 2; i < n; ++i)
            triangle(i % 2 ? i - 1 : i - 2, i % 2 ? i - 2 : i - 1, i);
        break;
    case Topology::TriangleFan:
    case Topology::Polygon:
        for (std::size_t i = 2; i < n; ++i)
            triangle(0, i - 1, i);
        break;
    case Topology::Quads:
        for (std::size_t i = 0; i + 3 < n; i += 4) {
            triangle(i, i + 1, i + 2);
            triangle(i, i + 2, i + 3);
        }
        break;
    case Topology::QuadStrip:
        // Match Eden's vulkan_quad_indexed.comp winding/diagonal.
        for (std::size_t i = 0; i + 3 < n; i += 2) {
            triangle(i, i + 3, i + 1);
            triangle(i, i + 2, i + 3);
        }
        break;
    }
}
template <typename Visit>
void Segments(Format format, std::span<const std::byte> input, Restart restart, Visit visit) {
    const auto width = Width(format);
    if (input.size() % width)
        throw std::invalid_argument("Truncated index input");
    const auto n = input.size() / width;
    if (!restart.enabled) {
        visit(0, n);
        return;
    }
    std::size_t begin{};
    for (std::size_t i = 0; i < n; ++i) {
        // Compare the zero-extended guest index to the full restart register.
        if (restart.enabled && Read(input, width, i) == restart.index) {
            visit(begin, i - begin);
            begin = i + 1;
        }
    }
    visit(begin, n - begin);
}
void ValidateOutput(const Layout& layout, std::span<std::byte> output) {
    if (output.size() < layout.bytes)
        throw std::out_of_range("Index output span is too small");
}
auto Writer(Format format, std::span<std::byte> output) {
    return [width = Width(format), output, cursor = std::size_t{}](std::uint32_t value) mutable {
        for (std::size_t b = 0; b < width; ++b)
            output[cursor++] = std::byte((value >> (8 * b)) & 255);
    };
}
} // namespace
Layout Analyze(Topology topology, Format format, std::span<const std::byte> input,
               Restart restart) {
    ListTopology(topology);
    std::uint64_t total{};
    Segments(format, input, restart, [&](std::size_t, std::size_t n) {
        const auto count = SegmentCount(topology, n);
        if (count > std::numeric_limits<std::uint32_t>::max() - total)
            throw std::overflow_error("Converted index count exceeds D3D12 limits");
        total += count;
    });
    return MakeLayout(topology, format == Format::Uint8 ? Format::Uint16 : format, total);
}
Layout AnalyzeGenerated(Topology topology, std::uint32_t n, std::uint32_t first) {
    if (n && std::uint64_t(first) + n - 1 > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Generated vertex index overflow");
    const auto format =
        !n || std::uint64_t(first) + n - 1 <= 65535 ? Format::Uint16 : Format::Uint32;
    return MakeLayout(topology, format, SegmentCount(topology, n));
}
Layout Convert(Topology topology, Format format, std::span<const std::byte> input,
               std::span<std::byte> output, Restart restart) {
    const auto layout = Analyze(topology, format, input, restart);
    ValidateOutput(layout, output);
    // Reject overlap before conversion; byte addresses avoid alignment assumptions.
    const auto src = reinterpret_cast<std::uintptr_t>(input.data());
    const auto dst = reinterpret_cast<std::uintptr_t>(output.data());
    if (layout.bytes && !input.empty() &&
        (dst >= src ? dst - src < input.size() : src - dst < layout.bytes))
        throw std::invalid_argument("Index input/output spans overlap");
    auto emit = Writer(layout.format, output);
    Segments(format, input, restart, [&](std::size_t begin, std::size_t n) {
        EmitSegment(
            topology, begin, n, [&](std::size_t i) { return Read(input, Width(format), i); },
            [&](std::uint32_t value) { emit(value); });
    });
    return layout;
}
Layout Generate(Topology topology, std::uint32_t n, std::span<std::byte> output,
                std::uint32_t first) {
    const auto layout = AnalyzeGenerated(topology, n, first);
    ValidateOutput(layout, output);
    EmitSegment(
        topology, 0, n, [&](std::size_t i) { return first + static_cast<std::uint32_t>(i); },
        Writer(layout.format, output));
    return layout;
}
} // namespace D3D12::IndexConversion

#ifdef _WIN32
namespace D3D12 {
IndexConverter::IndexConverter(Context& context)
    : context{context}, upload{context, D3D12_HEAP_TYPE_UPLOAD} {}
ConvertedIndexBuffer IndexConverter::Upload(const IndexConversion::Layout& layout,
                                            StagingBufferRef& allocation) {
    using namespace IndexConversion;
    const auto topology = layout.topology == Topology::Points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                          : layout.topology == Topology::Lines
                              ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                              : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (!layout.count)
        return {{}, topology, 0};
    context.Retain(allocation.buffer); // Fence-retired resource lease prevents ring reuse.
    return {{allocation.buffer->handle->GetGPUVirtualAddress() + allocation.offset,
             static_cast<UINT>(layout.bytes),
             layout.format == Format::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT},
            topology,
            layout.count};
}
ConvertedIndexBuffer IndexConverter::Convert(IndexConversion::Topology topology,
                                             IndexConversion::Format format,
                                             std::span<const std::byte> input,
                                             IndexConversion::Restart restart) {
    const auto layout = IndexConversion::Analyze(topology, format, input, restart);
    if (layout.bytes > UINT_MAX)
        throw std::overflow_error("Index buffer view exceeds UINT size");
    auto allocation = layout.bytes ? upload.Request(layout.bytes) : StagingBufferRef{};
    IndexConversion::Convert(topology, format, input,
                             std::as_writable_bytes(allocation.mapped_span), restart);
    return Upload(layout, allocation);
}
ConvertedIndexBuffer IndexConverter::Generate(IndexConversion::Topology topology, std::uint32_t n,
                                              std::uint32_t first) {
    const auto layout = IndexConversion::AnalyzeGenerated(topology, n, first);
    if (layout.bytes > UINT_MAX)
        throw std::overflow_error("Index buffer view exceeds UINT size");
    auto allocation = layout.bytes ? upload.Request(layout.bytes) : StagingBufferRef{};
    IndexConversion::Generate(topology, n, std::as_writable_bytes(allocation.mapped_span), first);
    return Upload(layout, allocation);
}
} // namespace D3D12
#endif
