// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

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
    switch (static_cast<std::uint32_t>(topology)) {
    case static_cast<std::uint32_t>(Topology::Points):
        return Topology::Points;
    case static_cast<std::uint32_t>(Topology::Lines):
    case static_cast<std::uint32_t>(Topology::LineLoop):
    case static_cast<std::uint32_t>(Topology::LineStrip):
        return Topology::Lines;
    case static_cast<std::uint32_t>(Topology::Triangles):
    case static_cast<std::uint32_t>(Topology::TriangleStrip):
    case static_cast<std::uint32_t>(Topology::TriangleFan):
    case static_cast<std::uint32_t>(Topology::Quads):
    case static_cast<std::uint32_t>(Topology::QuadStrip):
    case static_cast<std::uint32_t>(Topology::Polygon):
        return Topology::Triangles;
    case static_cast<std::uint32_t>(LinesAdjacency):
    case static_cast<std::uint32_t>(LineStripAdjacency):
        return LinesAdjacency;
    case static_cast<std::uint32_t>(TrianglesAdjacency):
    case static_cast<std::uint32_t>(TriangleStripAdjacency):
        return TrianglesAdjacency;
    }
    throw std::invalid_argument("Unsupported index topology");
}
std::uint64_t SegmentCount(Topology topology, std::uint64_t n) {
    // Even discarded tails cannot make a longer segment fit a UINT draw count.
    // Bound before multiplying so adversarial spans cannot wrap the count.
    if (n > std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 2)
        throw std::overflow_error("Index segment exceeds D3D12 limits");
    switch (static_cast<std::uint32_t>(topology)) {
    case static_cast<std::uint32_t>(Topology::Points):
        return n;
    case static_cast<std::uint32_t>(Topology::Lines):
        return n / 2 * 2;
    case static_cast<std::uint32_t>(Topology::LineStrip):
        return n >= 2 ? (n - 1) * 2 : 0;
    case static_cast<std::uint32_t>(Topology::LineLoop):
        return n >= 2 ? n * 2 : 0;
    case static_cast<std::uint32_t>(Topology::Triangles):
        return n / 3 * 3;
    case static_cast<std::uint32_t>(Topology::Quads):
        return n / 4 * 6;
    case static_cast<std::uint32_t>(Topology::QuadStrip):
        return n >= 4 ? (n / 2 - 1) * 6 : 0;
    case static_cast<std::uint32_t>(Topology::TriangleStrip):
    case static_cast<std::uint32_t>(Topology::TriangleFan):
    case static_cast<std::uint32_t>(Topology::Polygon):
        return n >= 3 ? (n - 2) * 3 : 0;
    case static_cast<std::uint32_t>(LinesAdjacency):
        return n / 4 * 4;
    case static_cast<std::uint32_t>(LineStripAdjacency):
        return n >= 4 ? (n - 3) * 4 : 0;
    case static_cast<std::uint32_t>(TrianglesAdjacency):
        return n / 6 * 6;
    case static_cast<std::uint32_t>(TriangleStripAdjacency):
        return n >= 6 ? (n / 2 - 2) * 6 : 0;
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
void EmitSegment(Topology topology, std::size_t begin, std::size_t n, ReadIndex read, Emit emit,
                 Provoking provoking) {
    auto at = [&](std::size_t i) { return read(begin + i); };
    auto line = [&](std::size_t a, std::size_t b) {
        emit(at(provoking == Provoking::Last ? b : a));
        emit(at(provoking == Provoking::Last ? a : b));
    };
    auto triangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        if (provoking == Provoking::Last) {
            emit(at(c));
            emit(at(a));
            emit(at(b));
        } else {
            emit(at(a));
            emit(at(b));
            emit(at(c));
        }
    };
    switch (static_cast<std::uint32_t>(topology)) {
    case static_cast<std::uint32_t>(Topology::Points):
        for (std::size_t i = 0; i < n; ++i)
            emit(at(i));
        break;
    case static_cast<std::uint32_t>(Topology::Lines):
        for (std::size_t i = 0; i + 1 < n; i += 2)
            line(i, i + 1);
        break;
    case static_cast<std::uint32_t>(Topology::LineStrip):
    case static_cast<std::uint32_t>(Topology::LineLoop):
        for (std::size_t i = 1; i < n; ++i)
            line(i - 1, i);
        if (topology == Topology::LineLoop && n >= 2)
            line(n - 1, 0);
        break;
    case static_cast<std::uint32_t>(Topology::Triangles):
        for (std::size_t i = 0; i + 2 < n; i += 3)
            triangle(i, i + 1, i + 2);
        break;
    case static_cast<std::uint32_t>(Topology::TriangleStrip):
        for (std::size_t i = 2; i < n; ++i) {
            if (provoking == Provoking::First && i % 2)
                triangle(i - 2, i, i - 1);
            else
                triangle(i % 2 ? i - 1 : i - 2, i % 2 ? i - 2 : i - 1, i);
        }
        break;
    case static_cast<std::uint32_t>(Topology::TriangleFan):
    case static_cast<std::uint32_t>(Topology::Polygon):
        for (std::size_t i = 2; i < n; ++i) {
            if (provoking == Provoking::First)
                triangle(i - 1, i, 0);
            else
                triangle(0, i - 1, i);
        }
        break;
    case static_cast<std::uint32_t>(Topology::Quads):
        for (std::size_t i = 0; i + 3 < n; i += 4) {
            triangle(i, i + 1, i + 2);
            triangle(i, i + 2, i + 3);
        }
        break;
    case static_cast<std::uint32_t>(Topology::QuadStrip):
        // Match Eden's vulkan_quad_indexed.comp winding/diagonal.
        for (std::size_t i = 0; i + 3 < n; i += 2) {
            if (provoking == Provoking::Last)
                triangle(i + 1, i, i + 3);
            else
                triangle(i, i + 3, i + 1);
            triangle(i, i + 2, i + 3);
        }
        break;
    case static_cast<std::uint32_t>(LinesAdjacency):
    case static_cast<std::uint32_t>(LineStripAdjacency):
        for (std::size_t i = 0; i + 3 < n; i += topology == LinesAdjacency ? 4 : 1)
            for (std::size_t j = 0; j < 4; ++j)
                emit(at(i + j));
        break;
    case static_cast<std::uint32_t>(TrianglesAdjacency):
        for (std::size_t i = 0; i + 5 < n; i += 6)
            for (std::size_t j = 0; j < 6; ++j)
                emit(at(i + j));
        break;
    case static_cast<std::uint32_t>(TriangleStripAdjacency): {
        // Vulkan drawing-triangle-strips-with-adjacency: edge neighbors differ
        // at the strip ends and on odd/even primitives. They are not a sliding
        // window of six vertices (even a single triangle swaps neighbors 3/5).
        const std::size_t count = n < 6 ? 0 : n / 2 - 2;
        for (std::size_t primitive = 0; primitive < count; ++primitive) {
            const auto base = primitive * 2;
            std::size_t indices[6];
            if (count == 1) {
                const std::size_t single[]{0, 1, 2, 5, 4, 3};
                for (std::size_t j = 0; j < 6; ++j)
                    indices[j] = single[j];
            } else if (primitive == 0) {
                const std::size_t first[]{0, 1, 2, 6, 4, 3};
                for (std::size_t j = 0; j < 6; ++j)
                    indices[j] = first[j];
            } else if (primitive % 2) {
                const std::size_t odd[]{base,     base + 3,
                                        base + 4, base + (primitive + 1 == count ? 5 : 6),
                                        base + 2, base - 2};
                for (std::size_t j = 0; j < 6; ++j)
                    indices[j] = odd[j];
            } else {
                const std::size_t even[]{base,     base - 2,
                                         base + 2, base + (primitive + 1 == count ? 5 : 6),
                                         base + 4, base + 3};
                for (std::size_t j = 0; j < 6; ++j)
                    indices[j] = even[j];
            }
            for (const auto index : indices)
                emit(at(index));
        }
        break;
    }
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
bool ContainsRestart(Format format, std::span<const std::byte> input, Restart restart) {
    const auto width = Width(format);
    if (input.size() % width)
        throw std::invalid_argument("Truncated index input");
    if (!restart.enabled || !RestartCanMatch(format, restart.index))
        return false;
    for (std::size_t i = 0; i < input.size() / width; ++i)
        if (Read(input, width, i) == restart.index)
            return true;
    return false;
}
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
               std::span<std::byte> output, Restart restart, Provoking provoking) {
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
            [&](std::uint32_t value) { emit(value); }, provoking);
    });
    return layout;
}
Layout Generate(Topology topology, std::uint32_t n, std::span<std::byte> output,
                std::uint32_t first, Provoking provoking) {
    const auto layout = AnalyzeGenerated(topology, n, first);
    ValidateOutput(layout, output);
    EmitSegment(
        topology, 0, n, [&](std::size_t i) { return first + static_cast<std::uint32_t>(i); },
        Writer(layout.format, output), provoking);
    return layout;
}
} // namespace D3D12::IndexConversion

#ifdef _WIN32
#include "common/cityhash.h"
namespace D3D12 {
IndexConverter::IndexConverter(Context& context)
    : context{context}, upload{context, D3D12_HEAP_TYPE_UPLOAD} {}
ConvertedIndexBuffer IndexConverter::Upload(const IndexConversion::Layout& layout,
                                            StagingBufferRef& allocation) {
    using namespace IndexConversion;
    const auto topology =
        layout.topology == Topology::Points     ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
        : layout.topology == LinesAdjacency     ? D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ
        : layout.topology == TrianglesAdjacency ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ
        : layout.topology == Topology::Lines    ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (!layout.count)
        return {{}, topology, 0};
    context.Profile().Count(FrameCount::BufferUploadBytes, layout.bytes);
    context.Profile().Count(FrameCount::IndexUploadBytes, layout.bytes);
    context.Retain(allocation.buffer); // Fence-retired resource lease prevents ring reuse.
    return {{allocation.buffer->handle->GetGPUVirtualAddress() + allocation.offset,
             static_cast<UINT>(layout.bytes),
             layout.format == Format::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT},
            topology,
            layout.count,
            false,
            allocation.buffer};
}
ConvertedIndexBuffer IndexConverter::Convert(IndexConversion::Topology topology,
                                             IndexConversion::Format format,
                                             std::span<const std::byte> input,
                                             IndexConversion::Restart restart,
                                             IndexConversion::Provoking provoking) {
    const auto layout = IndexConversion::Analyze(topology, format, input, restart);
    if (layout.bytes > UINT_MAX)
        throw std::overflow_error("Index buffer view exceeds UINT size");
    auto allocation = layout.bytes ? upload.Request(layout.bytes) : StagingBufferRef{};
    IndexConversion::Convert(topology, format, input,
                             std::as_writable_bytes(allocation.mapped_span), restart, provoking);
    return Upload(layout, allocation);
}
ConvertedIndexBuffer IndexConverter::ConvertCached(IndexConversion::CacheKey key,
                                                   std::span<const std::byte> input,
                                                   bool& cache_hit) {
    {
        const FrameProfile::Scope profile_hash{context.Profile(), FramePhase::IndexHash};
        key.content_hash =
            Common::CityHash64(reinterpret_cast<const char*>(input.data()), input.size());
    }
    if (const auto* found = cache.Find(key)) {
        cache_hit = true;
        context.Retain(found->resource);
        return *found;
    }
    cache_hit = false;
    const auto topology = static_cast<IndexConversion::Topology>(key.topology);
    const auto format = static_cast<IndexConversion::Format>(key.format);
    const IndexConversion::Restart restart{key.restart_enabled, key.restart_index};
    const auto provoking = static_cast<IndexConversion::Provoking>(key.provoking);
    const auto layout = IndexConversion::Analyze(topology, format, input, restart);
    if (layout.bytes > UINT_MAX)
        throw std::overflow_error("Index buffer view exceeds UINT size");
    // Dedicated upload allocation: no staging-ring page can overwrite cached bytes.
    StagingBufferRef allocation{};
    if (layout.bytes) {
        const FrameProfile::Scope profile_allocation{context.Profile(),
                                                     FramePhase::UploadAllocation};
        allocation.buffer = context.CreateBuffer(layout.bytes, D3D12_HEAP_TYPE_UPLOAD);
        allocation.mapped_span = allocation.buffer->mapped.first(layout.bytes);
    }
    IndexConversion::Convert(topology, format, input,
                             std::as_writable_bytes(allocation.mapped_span), restart, provoking);
    auto result = Upload(layout, allocation);
    // Charge the actual D3D12 allocation (including heap alignment), not just payload bytes.
    cache.Insert(key, result, result.resource ? result.resource->allocation_size : 0);
    return result;
}
std::optional<ConvertedIndexBuffer> IndexConverter::FindValidated(
    const IndexConversion::CacheKey& key, const ResourcePtr& source) {
    const auto* found = validated.Find(key);
    if (!found || !found->Matches(source))
        return std::nullopt;
    auto result = found->value.buffer;
    if (!result.native && result.count) {
        result.resource = found->value.allocation.lock();
        if (!result.resource)
            return std::nullopt; // Content-cache eviction requires fresh validation.
        context.Retain(result.resource);
    }
    return result;
}
void IndexConverter::RememberValidated(const IndexConversion::CacheKey& key,
                                       const ResourcePtr& source,
                                       const ConvertedIndexBuffer& result) {
    if (!source)
        return;
    auto metadata = result;
    metadata.resource.reset();
    validated.Insert(key, {source, source->content_version, {metadata, result.resource}},
                     sizeof(ValidatedIndexBuffer));
}
ConvertedIndexBuffer IndexConverter::Generate(IndexConversion::Topology topology, std::uint32_t n,
                                              std::uint32_t first,
                                              IndexConversion::Provoking provoking) {
    const auto layout = IndexConversion::AnalyzeGenerated(topology, n, first);
    if (layout.bytes > UINT_MAX)
        throw std::overflow_error("Index buffer view exceeds UINT size");
    auto allocation = layout.bytes ? upload.Request(layout.bytes) : StagingBufferRef{};
    IndexConversion::Generate(topology, n, std::as_writable_bytes(allocation.mapped_span), first,
                              provoking);
    return Upload(layout, allocation);
}
} // namespace D3D12
#endif
