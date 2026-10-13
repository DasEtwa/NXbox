// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// RasterizerD3D12::Draw(): call Convert for guest indices, Generate otherwise, before
// IASetIndexBuffer/DrawIndexedInstanced with the returned list topology/count.
// Set PSO topology type accordingly and DISABLE strip cut. Preserve the guest base
// vertex in DrawIndexedInstanced (not in these indices), plus instance/base-instance.
// CPU input must be coherent guest memory. Polygon means a convex polygon fan.

#include <cstddef>
#include <cstdint>
#include <span>

namespace D3D12 {
namespace IndexConversion {
// Numeric values match Maxwell3D::Regs::PrimitiveTopology and IndexFormat.
enum class Topology : std::uint32_t {
    Points = 0,
    Lines,
    LineLoop,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan,
    Quads,
    QuadStrip,
    Polygon,
};
inline constexpr Topology LinesAdjacency = static_cast<Topology>(10);
inline constexpr Topology LineStripAdjacency = static_cast<Topology>(11);
inline constexpr Topology TrianglesAdjacency = static_cast<Topology>(12);
inline constexpr Topology TriangleStripAdjacency = static_cast<Topology>(13);
// Preserve retains the legacy expansion for existing callers. First/Last select
// the guest flat-shading vertex while preserving triangle winding.
enum class Provoking { Preserve, First, Last };
enum class Format : std::uint32_t { Uint8 = 0, Uint16 = 1, Uint32 = 2 };
struct Restart {
    bool enabled{};
    std::uint32_t index{0xffffffff};
};
// Native lists already have the required order. Avoid a synchronous guest read,
// CPU expansion and staging upload when conversion would only copy the indices.
constexpr bool NeedsConversion(Topology topology, Format format, Restart restart,
                               Provoking provoking) {
    if ((format != Format::Uint16 && format != Format::Uint32) || restart.enabled)
        return true;
    if (topology == Topology::Points)
        return false;
    if (topology == Topology::Lines || topology == Topology::Triangles)
        return provoking == Provoking::Last;
    return true;
}
struct Layout {
    Topology topology; // List topology, including adjacency; no restart markers.
    Format format;     // Never Uint8.
    std::uint32_t count;
    std::size_t bytes;
};
// Packed little-endian spans may be unaligned. Incomplete primitives are discarded
// at each restart/end; degenerates are preserved. Input/output must not overlap.
// Analyze/AnalyzeGenerated give the exact allocation. Invalid enums, truncated
// input, overflow and undersized output throw before writing any output.
Layout Analyze(Topology topology, Format format, std::span<const std::byte> input,
               Restart restart = {});
Layout AnalyzeGenerated(Topology topology, std::uint32_t vertex_count,
                        std::uint32_t first_vertex = 0);
Layout Convert(Topology topology, Format format, std::span<const std::byte> input,
               std::span<std::byte> output, Restart restart = {},
               Provoking provoking = Provoking::Preserve);
Layout Generate(Topology topology, std::uint32_t vertex_count, std::span<std::byte> output,
                std::uint32_t first_vertex = 0, Provoking provoking = Provoking::Preserve);
} // namespace IndexConversion
} // namespace D3D12

#ifdef _WIN32
#include "video_core/renderer_d3d12/d3d12_context.h"
namespace D3D12 {
struct ConvertedIndexBuffer {
    D3D12_INDEX_BUFFER_VIEW view{};
    D3D_PRIMITIVE_TOPOLOGY topology{};
    std::uint32_t count{};
    ResourcePtr resource; // Keep the upload page leased across cache submissions before the draw.
};
class IndexConverter {
public:
    explicit IndexConverter(Context& context);
    ConvertedIndexBuffer Convert(
        IndexConversion::Topology topology, IndexConversion::Format format,
        std::span<const std::byte> input, IndexConversion::Restart restart = {},
        IndexConversion::Provoking provoking = IndexConversion::Provoking::Preserve);
    ConvertedIndexBuffer Generate(
        IndexConversion::Topology topology, std::uint32_t vertex_count,
        std::uint32_t first_vertex = 0,
        IndexConversion::Provoking provoking = IndexConversion::Provoking::Preserve);

private:
    ConvertedIndexBuffer Upload(const IndexConversion::Layout& layout,
                                StagingBufferRef& allocation);
    Context& context;
    StagingRing upload;
};
} // namespace D3D12
#endif
