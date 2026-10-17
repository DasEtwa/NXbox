// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// RasterizerD3D12::Clear(layer_count): decode regs.clear_surface.raw and
// regs.clear_control (respect_stencil_mask), and supply clear values plus resolved
// RTV/DSV views for exactly clear_surface.layer..layer+layer_count. Call Execute
// once per layer with single-slice views (also for masked clears).
// Construct ClearRenderer at rasterizer startup to compile built-in DXIL.
// Intersect the render area with scissor 0 only when clear_control.use_scissor
// AND scissor_test[0].enable; apply guest Y orientation/resolution scaling first.
// Also intersect clear_control.use_clear_rect/use_viewport_clip0 when enabled.
// Execute changes PSO/root signature/OM/IA/viewport/scissor/stencil-reference:
// invalidate cached graphics state before the next Draw(). Keep CPU descriptors
// alive until the Context fence completes. Resource transitions/retention are automatic.

#include <array>
#include <cstdint>

namespace D3D12 {
namespace ClearOperations {
struct Rect {
    std::int32_t left{}, top{}, right{}, bottom{};
    bool Empty() const {
        return left >= right || top >= bottom;
    }
};
Rect Intersect(Rect area, Rect clip);
// Maxwell use_scissor is clear_control bit 8; a disabled scissor is unrestricted.
Rect ResolveScissor(Rect render_area, std::uint32_t clear_control, bool scissor_enabled,
                    Rect scissor);
struct Selection {
    std::uint8_t color_mask{}; // D3D12 RGBA write-mask bits.
    bool depth{}, stencil{};
    std::uint8_t stencil_mask{255};
    std::uint32_t target{}, layer{};
};
// clear_surface: Z/S/R/G/B/A bits 0..5, RT bits 6..9, layer bits 10..25.
// clear_control bit 0 requests the stencil write mask; otherwise all bits clear.
Selection Decode(std::uint32_t clear_surface, std::uint32_t clear_control,
                 std::uint32_t stencil_write_mask);
} // namespace ClearOperations
} // namespace D3D12

#ifdef _WIN32
#include <memory>
#include "video_core/renderer_d3d12/d3d12_context.h"
namespace D3D12 {
enum class ClearColorType { Float, Uint, Sint };
struct ClearAttachment {
    ResourcePtr resource;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}; // Typed RTV/DSV format, not resource format.
    std::uint32_t width{}, height{};         // View mip extent.
    DXGI_SAMPLE_DESC samples{1, 0};
    bool has_depth{}, has_stencil{}; // DSV aspects; unused for RTVs.
};
struct ClearValues {
    std::array<float, 4> color{};
    // Exact integer RTV values, already converted from guest clear_color by caller.
    std::array<std::uint32_t, 4> integer_color{}; // Two's-complement bits for Sint.
    ClearColorType color_type{ClearColorType::Float};
    float depth{1};
    std::uint8_t stencil{};
};
class ClearRenderer {
public:
    explicit ClearRenderer(Context& context);
    ~ClearRenderer();
    // Null attachments are ignored. clip is in host coordinates and is intersected
    // independently with each view extent. Returns true if shader state was bound.
    bool Execute(const ClearOperations::Selection& selection, const ClearValues& values,
                 const ClearAttachment* color, const ClearAttachment* depth_stencil,
                 const ClearOperations::Rect* clip = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12
#endif
