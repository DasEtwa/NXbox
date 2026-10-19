// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Maxwell register values -> D3D12 enum values, and the packed graphics PSO key.
// Everything here is std-only and uses raw numeric enum values (Maxwell regs on the input side,
// D3D12/DXGI SDK values on the output side) so it can be unit tested without a Windows SDK.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace D3D12::Pso {

// ---- D3D12 SDK values (d3d12.h / dxgiformat.h) used by the tables --------------------------
inline constexpr std::uint32_t kBlendZero = 1, kBlendOne = 2, kBlendAlphaFactor = 20,
                               kBlendInvAlphaFactor = 21, kBlendFactor = 14, kBlendInvFactor = 15;
inline constexpr std::uint32_t kCullNone = 1, kCullFront = 2, kCullBack = 3;
inline constexpr std::uint32_t kTopologyTypePoint = 1, kTopologyTypeLine = 2,
                               kTopologyTypeTriangle = 3, kTopologyTypePatch = 4;

// ---- Blend / compare / stencil -------------------------------------------------------------
// D3D12_BLEND from Maxwell::Blend::Factor (D3D and GL numbering). Unknown -> ONE.
constexpr std::uint32_t BlendFactor(std::uint32_t v) {
    if (v >= 0x1 && v <= 0xB)
        return v; // Zero..SrcAlphaSat share D3D12 numbering
    switch (v) {
    case 0xC:
        return 5; // BothSourceAlpha (D3D9) -> SRC_ALPHA
    case 0xD:
        return 6; // OneMinusBothSourceAlpha -> INV_SRC_ALPHA
    case 0xE:
        return kBlendFactor;
    case 0xF:
        return kBlendInvFactor;
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
        return v; // SRC1_*
    case 0x4000:
        return 1;
    case 0x4001:
        return 2;
    case 0x4300:
    case 0x4301:
    case 0x4302:
    case 0x4303:
    case 0x4304:
    case 0x4305:
    case 0x4306:
    case 0x4307:
    case 0x4308:
        return v - 0x4300 + 3;
    case 0xC001:
        return kBlendFactor;
    case 0xC002:
        return kBlendInvFactor;
    case 0xC003:
        return kBlendAlphaFactor;
    case 0xC004:
        return kBlendInvAlphaFactor;
    case 0xC900:
    case 0xC901:
    case 0xC902:
    case 0xC903:
        return v - 0xC900 + 16;
    default:
        return kBlendOne;
    }
}

// D3D12 alpha equations forbid *_COLOR factors; Maxwell evaluates their
// alpha component. SRC_ALPHA_SAT has an alpha factor of one.
constexpr std::uint32_t AlphaBlendFactor(std::uint32_t value) {
    switch (const auto factor = BlendFactor(value)) {
    case 3:
        return 5;
    case 4:
        return 6;
    case 9:
        return 7;
    case 10:
        return 8;
    case 11:
        return kBlendOne;
    case 16:
        return 18;
    case 17:
        return 19;
    case kBlendAlphaFactor:
        return kBlendFactor;
    case kBlendInvAlphaFactor:
        return kBlendInvFactor;
    default:
        return factor;
    }
}

// D3D12_BLEND_OP from Maxwell::Blend::Equation. Unknown -> ADD.
constexpr std::uint32_t BlendOp(std::uint32_t v) {
    if (v >= 1 && v <= 5)
        return v;
    switch (v) {
    case 0x8006:
        return 1; // Add
    case 0x800A:
        return 2; // Subtract
    case 0x800B:
        return 3; // ReverseSubtract
    case 0x8007:
        return 4; // Min
    case 0x8008:
        return 5; // Max
    default:
        return 1;
    }
}

// GL logic-op order differs from the D3D12 SDK order.
constexpr std::uint32_t LogicOp(std::uint32_t value) {
    constexpr std::array<std::uint8_t, 16> ops{0, 6,  12, 2,  13, 4,  10, 8,
                                               9, 11, 5,  14, 3,  15, 7,  1};
    return value >= 0x1500 && value <= 0x150f ? ops[value - 0x1500] : 4;
}

// Guest indirect stride zero means tightly packed. Reject truncated/overflowing
// ranges before accessing memory or issuing any draw.
constexpr std::uint64_t IndirectCount(std::uint64_t bytes, std::uint64_t stride,
                                      std::uint64_t command_size, std::uint64_t maximum,
                                      std::uint64_t count) {
    stride = stride ? stride : command_size;
    if (!command_size || stride < command_size || bytes < command_size)
        return 0;
    const auto capacity = 1 + (bytes - command_size) / stride;
    return (std::min)((std::min)(maximum, count), capacity);
}

// D3D12_COMPARISON_FUNC from Maxwell::ComparisonOp. Unknown -> ALWAYS.
constexpr std::uint32_t ComparisonFunc(std::uint32_t v) {
    if (v >= 1 && v <= 8)
        return v;
    if (v >= 0x200 && v <= 0x207)
        return v - 0x200 + 1;
    return 8;
}

// D3D12_STENCIL_OP from Maxwell::StencilOp::Op. Unknown -> KEEP.
constexpr std::uint32_t StencilOp(std::uint32_t v) {
    if (v >= 1 && v <= 8)
        return v;
    switch (v) {
    case 0x1E00:
        return 1; // Keep
    case 0:
        return 2; // Zero_GL
    case 0x1E01:
        return 3; // Replace
    case 0x1E02:
        return 4; // IncrSaturate
    case 0x1E03:
        return 5; // DecrSaturate
    case 0x150A:
        return 6; // Invert
    case 0x8507:
        return 7; // Incr (wrap)
    case 0x8508:
        return 8; // Decr (wrap)
    default:
        return 1;
    }
}

// D3D12_CULL_MODE from the GL cull test + face. FrontAndBack is reported through `cull_all`
// (D3D12 has no such mode; the draw must be skipped).
struct CullInfo {
    std::uint32_t mode;
    bool cull_all;
};
constexpr CullInfo CullMode(bool enabled, std::uint32_t maxwell_face) {
    if (!enabled)
        return {kCullNone, false};
    switch (maxwell_face) {
    case 0x404:
        return {kCullFront, false};
    case 0x408:
        return {kCullBack, true};
    default:
        return {kCullBack, false};
    }
}

// The legacy API shares reference/compare/write masks between the two faces.
// Single-face culling can select that face's shared state without new interfaces.
constexpr bool IndependentStencil(bool two_sided, std::uint32_t cull_mode, std::uint32_t front_ref,
                                  std::uint32_t back_ref, std::uint32_t front_read,
                                  std::uint32_t back_read, std::uint32_t front_write,
                                  std::uint32_t back_write) {
    return two_sided && cull_mode == kCullNone &&
           (((front_ref ^ back_ref) | (front_read ^ back_read) | (front_write ^ back_write)) &
            255) != 0;
}

// ---- Topology ------------------------------------------------------------------------------
// D3D12_PRIMITIVE_TOPOLOGY_TYPE for the PSO from Maxwell::PrimitiveTopology.
constexpr std::uint32_t TopologyType(std::uint32_t t) {
    switch (t) {
    case 0x0:
        return kTopologyTypePoint;
    case 0x1:
    case 0x2:
    case 0x3:
    case 0xA:
    case 0xB:
        return kTopologyTypeLine;
    case 0xE:
        return kTopologyTypePatch;
    default:
        return kTopologyTypeTriangle; // triangles, strips, fans, quads, polygon, adjacency
    }
}

// D3D_PRIMITIVE_TOPOLOGY for IASetPrimitiveTopology. `needs_conversion` means the guest
// topology has no D3D12 equivalent (LineLoop, Fan, Quads, QuadStrip, Polygon): the rasterizer
// must expand the index buffer and draw with the returned list/strip topology.
// Match GraphicsRuntime: disabled and ALWAYS alpha tests emit no HLSL test.
constexpr std::array<std::uint32_t, 2> AlphaTestSpecialization(bool enabled, std::uint32_t function,
                                                               std::uint32_t reference) {
    return enabled && function != 8 ? std::array{function, reference}
                                    : std::array<std::uint32_t, 2>{};
}
struct TopologyInfo {
    std::uint32_t topology;
    bool needs_conversion;
};
constexpr TopologyInfo PrimitiveTopology(std::uint32_t t, std::uint32_t patch_vertices = 3) {
    switch (t) {
    case 0x0:
        return {1, false}; // POINTLIST
    case 0x1:
        return {2, false}; // LINELIST
    case 0x2:
        return {3, true}; // LineLoop -> LINESTRIP + closing index
    case 0x3:
        return {3, false}; // LINESTRIP
    case 0x4:
        return {4, false}; // TRIANGLELIST
    case 0x5:
        return {5, false}; // TRIANGLESTRIP
    case 0x6:
        return {4, true}; // Fan
    case 0x7:
        return {4, true}; // Quads
    case 0x8:
        return {4, true}; // QuadStrip
    case 0x9:
        return {4, true}; // Polygon
    case 0xA:
        return {10, false}; // LINELIST_ADJ
    case 0xB:
        return {11, false}; // LINESTRIP_ADJ
    case 0xC:
        return {12, false}; // TRIANGLELIST_ADJ
    case 0xD:
        return {13, false}; // TRIANGLESTRIP_ADJ
    case 0xE:
        return {32 + (patch_vertices < 1    ? 1
                      : patch_vertices > 32 ? 32
                                            : patch_vertices),
                false}; // n_CONTROL_POINT_PATCHLIST
    default:
        return {4, false};
    }
}

// ---- Vertex formats ------------------------------------------------------------------------
// Maxwell::VertexAttribute::Size / Type numeric values.
namespace VSize {
inline constexpr std::uint32_t R32G32B32A32 = 0x01, R32G32B32 = 0x02, R16G16B16A16 = 0x03,
                               R32G32 = 0x04, R16G16B16 = 0x05, R8G8B8A8 = 0x0A, R16G16 = 0x0F,
                               R32 = 0x12, R8G8B8 = 0x13, R8G8 = 0x18, R16 = 0x1B, R8 = 0x1D,
                               A2B10G10R10 = 0x30, B10G11R11 = 0x31, G8R8 = 0x32, X8B8G8R8 = 0x33,
                               A8 = 0x34;
}
namespace VType {
inline constexpr std::uint32_t SNorm = 1, UNorm = 2, SInt = 3, UInt = 4, UScaled = 5, SScaled = 6,
                               Float = 7;
}

constexpr std::uint32_t VertexByteSize(std::uint32_t size) {
    using namespace VSize;
    switch (size) {
    case R32G32B32A32:
        return 16;
    case R32G32B32:
        return 12;
    case R16G16B16A16:
    case R32G32:
        return 8;
    case R16G16B16:
        return 6;
    case R8G8B8:
        return 3;
    case R16:
    case R8G8:
    case G8R8:
        return 2;
    case R8:
    case A8:
        return 1;
    default:
        return 4;
    }
}
constexpr std::uint32_t VertexAlignment(std::uint32_t size) {
    // D3D12 input elements require min(format width, 4) byte alignment,
    // including four-byte alignment for R8G8B8A8.
    const auto bytes = VertexByteSize(size);
    return bytes >= 4 ? 4 : bytes == 3 ? 1 : bytes;
}
constexpr std::uint64_t VertexLastNonIndexed(std::uint32_t first, std::uint32_t count) {
    return std::uint64_t{first} + (count ? count - 1 : 0);
}
constexpr std::uint64_t VertexRequiredBytes(std::uint64_t last, std::uint32_t stride,
                                            std::uint32_t bytes) {
    return last * stride + bytes;
}

inline void RepackVertexAttribute(std::span<std::uint8_t> destination,
                                  std::span<const std::uint8_t> source, std::uint32_t stride,
                                  std::uint32_t bytes) {
    std::fill(destination.begin(), destination.end(), std::uint8_t{0});
    if (!bytes)
        return;
    for (std::size_t element = 0; element < destination.size() / bytes; ++element) {
        const std::uint64_t offset = std::uint64_t{element} * stride;
        if (offset >= source.size())
            break;
        const auto count = std::min<std::uint64_t>(bytes, source.size() - offset);
        std::copy_n(source.data() + offset, static_cast<std::size_t>(count),
                    destination.data() + element * bytes);
    }
}

constexpr std::uint32_t VertexComponentBytes(std::uint32_t size) {
    using namespace VSize;
    if (size == R32 || size == R32G32 || size == R32G32B32 || size == R32G32B32A32)
        return 4;
    if (size == R16 || size == R16G16 || size == R16G16B16 || size == R16G16B16A16)
        return 2;
    return 1;
}
constexpr std::uint32_t VertexStorageByteSize(std::uint32_t size, bool swap_rb = false) {
    if (swap_rb && size != VSize::A2B10G10R10 && size != VSize::B10G11R11)
        return VertexComponentBytes(size) * 4;
    if (size == VSize::R16G16B16)
        return 8;
    if (size == VSize::R8G8B8)
        return 4;
    return VertexByteSize(size);
}
inline void RepackVertexFormat(std::span<std::uint8_t> destination,
                               std::span<const std::uint8_t> source, std::uint32_t stride,
                               std::uint32_t size, std::uint32_t type, bool swap_rb) {
    const auto guest_bytes = VertexByteSize(size);
    const auto storage_bytes = VertexStorageByteSize(size, swap_rb);
    std::fill(destination.begin(), destination.end(), std::uint8_t{0});
    for (std::size_t element = 0; element < destination.size() / storage_bytes; ++element) {
        const std::uint64_t offset = std::uint64_t{element} * stride;
        auto* target = destination.data() + element * storage_bytes;
        if (offset < source.size())
            std::copy_n(source.data() + offset,
                        static_cast<std::size_t>(
                            std::min<std::uint64_t>(guest_bytes, source.size() - offset)),
                        target);
        const auto component_bytes = VertexComponentBytes(size);
        if (storage_bytes > guest_bytes) {
            const std::uint32_t one =
                type == VType::Float   ? (component_bytes == 2 ? 0x3c00u : 0x3f800000u)
                : type == VType::UNorm ? (component_bytes == 2 ? 0xffffu : 0xffu)
                : type == VType::SNorm ? (component_bytes == 2 ? 0x7fffu : 0x7fu)
                                       : 1u;
            std::memcpy(target + storage_bytes - component_bytes, &one, component_bytes);
        }
        if (swap_rb && size == VSize::A2B10G10R10) {
            std::uint32_t packed{};
            std::memcpy(&packed, target, sizeof(packed));
            packed = (packed & 0xc00ffc00u) | ((packed & 0x3ffu) << 20) | ((packed >> 20) & 0x3ffu);
            std::memcpy(target, &packed, sizeof(packed));
        } else if (swap_rb && storage_bytes >= 3) {
            for (std::uint32_t byte = 0; byte < component_bytes; ++byte)
                std::swap(target[byte], target[component_bytes * 2 + byte]);
        }
    }
}

struct VertexFormat {
    std::uint32_t dxgi; // DXGI_FORMAT, 0 = unsupported
    bool scaled;        // integer in memory, shader must convert to float (U/SScaled)
    bool swap_rb;       // BGRA order the DXGI format cannot express; shader swizzles
};

// Maxwell (size, type, bgra) -> DXGI_FORMAT. Three-component 8/16-bit entries
// identify widened storage formats, requiring repacking/vertex pulling before
// IA use. Keeping the guest stride alone would read an incorrect fourth component.
constexpr VertexFormat VertexFormatFor(std::uint32_t size, std::uint32_t type, bool bgra = false) {
    using namespace VSize;
    const bool scaled = type == VType::UScaled || type == VType::SScaled;
    // Scaled types ride on the matching integer format.
    const std::uint32_t t = type == VType::UScaled   ? VType::UInt
                            : type == VType::SScaled ? VType::SInt
                                                     : type;
    const auto pick = [&](std::uint32_t snorm, std::uint32_t unorm, std::uint32_t sint,
                          std::uint32_t uint, std::uint32_t flt) -> std::uint32_t {
        switch (t) {
        case VType::SNorm:
            return snorm;
        case VType::UNorm:
            return unorm;
        case VType::SInt:
            return sint;
        case VType::UInt:
            return uint;
        case VType::Float:
            return flt;
        default:
            return 0;
        }
    };
    std::uint32_t f = 0;
    switch (size) {
    case R32G32B32A32:
        f = pick(0, 0, 4, 3, 2);
        break;
    case R32G32B32:
        f = pick(0, 0, 8, 7, 6);
        break;
    case R32G32:
        f = pick(0, 0, 18, 17, 16);
        break;
    case R32:
        f = pick(0, 0, 43, 42, 41);
        break;
    case R16G16B16A16:
    case R16G16B16:
        f = pick(13, 11, 14, 12, 10);
        break;
    case R16G16:
        f = pick(37, 35, 38, 36, 34);
        break;
    case R16:
        f = pick(58, 56, 59, 57, 54);
        break;
    case R8G8B8A8:
    case R8G8B8:
    case X8B8G8R8:
        f = pick(31, 28, 32, 30, 0);
        break;
    case R8G8:
    case G8R8:
        f = pick(51, 49, 52, 50, 0);
        break;
    case R8:
        f = pick(63, 61, 64, 62, 0);
        break;
    case A8:
        f = t == VType::UNorm ? 65 : t == VType::UInt ? 62 : 0;
        break; // A8_UNORM / R8_UINT
    case A2B10G10R10:
        // SNorm/SInt fetch the raw UINT components; the HLSL vertex shader sign-extends
        // and normalizes them (AttributeType::SignedNorm10 / SignedInt10). SScaled would
        // read negatives as large positives through the scaled float() path, so reject it.
        f = type == VType::SScaled ? 0 : pick(25, 24, 25, 25, 0);
        break;
    case B10G11R11:
        f = t == VType::Float ? 26 : 0;
        break;
    default:
        break;
    }
    // BGRA only has a DXGI spelling for 4x8 UNorm; everything else swizzles in the shader.
    if (bgra && f == 28)
        return {87, scaled, false}; // B8G8R8A8_UNORM
    return {f, scaled, bgra && f != 0};
}

// IA formats are distinct from texture/view formats. The CPU repacker and HLSL
// signatures implement the conversion policy represented by these flags.
constexpr VertexFormat VertexFormatForInputAssembler(std::uint32_t size, std::uint32_t type,
                                                     bool bgra = false) {
    // Maxwell aliases follow Vulkan/GL channel counts: A8 is a one-component
    // R8 fetch, G8_R8 is RG, and X8_B8_G8_R8 carries all four raw components.
    auto format = VertexFormatFor(size == VSize::A8 ? VSize::R8 : size, type, bgra);
    if (format.dxgi == 87) {
        format.dxgi = 28;
        format.swap_rb = true;
    }
    if (format.swap_rb && size == VSize::B10G11R11)
        return {}; // Unequal-width packed BGRA needs conversion to floating components.
    if (format.swap_rb && size != VSize::A2B10G10R10) {
        const auto width = VertexComponentBytes(size);
        const auto expanded = width == 4   ? VSize::R32G32B32A32
                              : width == 2 ? VSize::R16G16B16A16
                                           : VSize::R8G8B8A8;
        format.dxgi = VertexFormatFor(expanded, type).dxgi;
    }
    return format;
}

// Signed packed 10:10:10:2 attributes are fetched as R10G10B10A2_UINT and decoded in HLSL.
constexpr bool VertexSignedPacked10(std::uint32_t size, std::uint32_t type) {
    return size == VSize::A2B10G10R10 && (type == VType::SNorm || type == VType::SInt);
}

// DXGI depth-stencil view format from the typeless resource format used by the texture cache
// (FormatInfo::resource_format). 0 = not a depth format / unsupported.
constexpr std::uint32_t DsvFormat(std::uint32_t resource_format) {
    switch (resource_format) {
    case 39:
        return 40; // R32_TYPELESS -> D32_FLOAT
    case 53:
        return 55; // R16_TYPELESS -> D16_UNORM
    case 44:
        return 45; // R24G8_TYPELESS -> D24_UNORM_S8_UINT
    case 19:
        return 20; // R32G8X24_TYPELESS -> D32_FLOAT_S8X24_UINT
    default:
        return 0;
    }
}

constexpr bool DsvHasStencil(std::uint32_t format) {
    return format == 20 || format == 45; // D32_FLOAT_S8X24_UINT / D24_UNORM_S8_UINT
}

// ---- Packed graphics state key -------------------------------------------------------------
struct BlendTargetState {
    std::uint8_t enable, write_mask, src, dst, op, src_alpha, dst_alpha, op_alpha;
};
struct StencilFaceState {
    std::uint8_t fail, depth_fail, pass, func;
};
struct VertexAttributeState {
    std::uint8_t enabled;
    std::uint8_t slot;
    std::uint16_t offset;
    std::uint32_t dxgi;
    std::uint8_t instanced;
    std::uint8_t swap_rb;
    std::uint8_t scaled;
    std::uint8_t pad;
    std::uint32_t divisor{1};
};

inline constexpr std::size_t kNumRenderTargets = 8;
inline constexpr std::size_t kNumVertexAttributes = 32;

constexpr bool RenderTargetLayersOverlap(std::uint32_t first_a, std::uint32_t count_a,
                                         std::uint32_t first_b, std::uint32_t count_b) {
    return count_a && count_b && std::uint64_t{first_a} < std::uint64_t{first_b} + count_b &&
           std::uint64_t{first_b} < std::uint64_t{first_a} + count_a;
}

// Actual bound views, in fragment-output slot order. Zero means unbound.
struct FramebufferFormats {
    std::array<std::uint32_t, kNumRenderTargets> colors{};
    std::uint32_t depth{};
    std::uint8_t sample_count{1};
};

// All values already in D3D12 numbering; no padding (checked below), so it is hashed and
// compared as raw bytes like Vulkan's FixedPipelineState.
struct GraphicsPsoState {
    std::uint8_t topology_type, cull_mode, fill_mode, front_ccw;
    std::uint8_t cull_all, depth_clip, depth_enable, depth_write;
    std::uint8_t depth_func, stencil_enable, stencil_read_mask, stencil_write_mask;
    std::uint8_t independent_blend, num_render_targets, sample_count, patch_vertices;
    std::uint8_t logic_enable, logic_op, alpha_to_coverage, conservative_raster;
    std::uint8_t smooth_lines, depth_bounds, stencil_back_read_mask, stencil_back_write_mask;
    std::int32_t depth_bias;
    std::uint32_t slope_scaled_bias_bits, bias_clamp_bits;
    std::uint32_t dsv_format;
    std::array<std::uint32_t, kNumRenderTargets> rtv_formats;
    StencilFaceState front, back;
    std::array<BlendTargetState, kNumRenderTargets> blend;
    std::array<VertexAttributeState, kNumVertexAttributes> attributes;
};
// Normalize fixed-function don't-cares and unused instance/patch rates. Preserve
// attribute binding metadata and topology consumed by the rasterizer.
constexpr void NormalizeGraphicsState(GraphicsPsoState& state) {
    if (!state.depth_enable) {
        state.depth_write = 0;
        state.depth_func = 8; // ALWAYS
    }
    if (!state.stencil_enable) {
        state.stencil_read_mask = state.stencil_write_mask = 0;
        state.stencil_back_read_mask = state.stencil_back_write_mask = 0;
        state.front = state.back = {1, 1, 1, 8}; // KEEP / ALWAYS
    }
    if (!(state.stencil_enable & 2))
        state.stencil_back_read_mask = state.stencil_back_write_mask = 0;
    if (state.topology_type != kTopologyTypePatch)
        state.patch_vertices = 0;
    for (auto& attribute : state.attributes)
        if (!attribute.instanced)
            attribute.divisor = 0;
    if (!state.logic_enable)
        state.logic_op = 4; // NOOP
    for (std::size_t i = 0; i < state.blend.size(); ++i) {
        auto& blend = state.blend[i];
        // With independent blending disabled, target zero controls every bound RTV.
        const bool used = (state.independent_blend || i == 0) && i < state.num_render_targets &&
                          state.rtv_formats[i];
        if (used) {
            if (!blend.enable) {
                blend.src = blend.src_alpha = kBlendOne;
                blend.dst = blend.dst_alpha = kBlendZero;
                blend.op = blend.op_alpha = 1;
            }
        } else if (!state.independent_blend && i != 0) {
            blend = state.blend[0];
        } else if (state.independent_blend) {
            blend = {0, 0, kBlendOne, kBlendZero, 1, kBlendOne, kBlendZero, 1};
        }
    }
}

// Only bound, blending attachments consume the dynamic constant. Bit 0 means
// RGB constant color; bit 1 means RGB constant alpha. Alpha equations always
// use the A component and are unaffected by replicating A across RGB.
constexpr std::uint32_t ConstantBlendFactors(const GraphicsPsoState& state) {
    std::uint32_t factors{};
    for (std::size_t i = 0; i < state.blend.size(); ++i) {
        if (!state.rtv_formats[i] || !state.blend[i].enable)
            continue;
        for (const auto factor : {state.blend[i].src, state.blend[i].dst}) {
            if (factor == kBlendFactor || factor == kBlendInvFactor)
                factors |= 1;
            if (factor == kBlendAlphaFactor || factor == kBlendInvAlphaFactor)
                factors |= 2;
        }
    }
    return factors;
}
constexpr void EmulateConstantAlpha(GraphicsPsoState& state) {
    for (auto& blend : state.blend) {
        for (auto* factor : {&blend.src, &blend.dst}) {
            if (*factor == kBlendAlphaFactor)
                *factor = kBlendFactor;
            if (*factor == kBlendInvAlphaFactor)
                *factor = kBlendInvFactor;
        }
    }
}

// Ignore trailing unbound slots, but preserve holes between fragment outputs.
constexpr void ApplyFramebufferFormats(GraphicsPsoState& state, const FramebufferFormats& formats) {
    state.rtv_formats = formats.colors;
    state.dsv_format = formats.depth;
    state.sample_count = formats.sample_count;
    state.num_render_targets = 0;
    for (std::size_t i = 0; i < formats.colors.size(); ++i)
        if (formats.colors[i])
            state.num_render_targets = static_cast<std::uint8_t>(i + 1);
    if (!formats.depth)
        state.depth_enable = state.depth_write = state.stencil_enable = state.depth_bounds = 0;
}

// Only shader reads enter the layout. Missing attributes use a free slot with stride zero.
constexpr void ApplyVertexReads(GraphicsPsoState& state, std::uint32_t reads) {
    std::array<bool, kNumVertexAttributes> used{};
    for (std::size_t i = 0; i < state.attributes.size(); ++i) {
        auto& a = state.attributes[i];
        if (!(reads & (std::uint32_t{1} << i)))
            a = {};
        else if (a.enabled)
            used[a.slot] = true;
    }
    std::uint8_t zero_slot = 0;
    while (zero_slot < used.size() && used[zero_slot])
        ++zero_slot;
    for (std::size_t i = 0; i < state.attributes.size(); ++i)
        if ((reads & (std::uint32_t{1} << i)) && !state.attributes[i].enabled)
            state.attributes[i] = {2, zero_slot, 0, 2, 0, 0, 0, 0, 0}; // R32G32B32A32_FLOAT
}
static_assert(std::has_unique_object_representations_v<GraphicsPsoState>);
static_assert(std::is_trivially_copyable_v<GraphicsPsoState>);

constexpr std::uint64_t HashBytes(const void* data, std::size_t size,
                                  std::uint64_t seed = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// Recipe v4 stores IA topology at bytes 8..11. It is dynamic command-list
// state, never part of the native PSO. Shader specialization remains in HLSL.
inline std::array<std::uint64_t, 2> NativeGraphicsRecipeIdentity(
    std::span<const std::uint8_t> recipe) {
    std::array<std::uint64_t, 2> hashes{0xcbf29ce484222325ull, 0x9e3779b97f4a7c15ull};
    for (std::size_t i = 0; i < recipe.size(); ++i) {
        if (i >= 8 && i < 12)
            continue;
        for (auto& hash : hashes)
            hash = (hash ^ recipe[i]) * 0x100000001b3ull;
    }
    return hashes;
}

// The seed is part of the input. Exact byte comparison avoids stale specialization hashes
// without relying on register dirty flags shared with other renderer consumers.
template <std::size_t Capacity>
class ByteHashCache {
public:
    std::uint64_t Get(const void* data, std::size_t size, std::uint64_t seed) {
        if (size > Capacity)
            return HashBytes(data, size, seed);
        if (valid && size == cached_size && seed == cached_seed &&
            std::memcmp(bytes.data(), data, size) == 0)
            return hash;
        hash = HashBytes(data, size, seed);
        std::memcpy(bytes.data(), data, size);
        cached_size = size;
        cached_seed = seed;
        valid = true;
        return hash;
    }

private:
    std::array<std::uint8_t, Capacity> bytes{};
    std::size_t cached_size{};
    std::uint64_t cached_seed{}, hash{};
    bool valid{};
};

} // namespace D3D12::Pso
