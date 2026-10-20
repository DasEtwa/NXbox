// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Shader::Backend::HLSL {
inline constexpr std::uint32_t ROOT_CONSTANT_DWORDS = 16;
// b0, space1. Upload all 16 DWORDs, including reserved fields. Scaling masks
// use per-stage texture descriptor indices and global UAV registers for images.
// Graphics texture masks occupy reserved[stage]; compute uses texture_scaling_mask.
struct RootConstants {
    float render_area[4]{0, 0, 1, 1};     // DWORD 0..3: guest render-area transform
    float resolution_down_factor{1};      // DWORD 4
    float y_direction{1};                 // DWORD 5
    std::uint32_t texture_scaling_mask{}; // DWORD 6
    std::uint32_t image_scaling_mask{};   // DWORD 7
    std::uint32_t base_vertex{};          // DWORD 8, signed two's-complement bits
    std::uint32_t base_instance{};        // DWORD 9
    std::uint32_t draw_id{};              // DWORD 10
    std::uint32_t reserved[5]{};          // DWORD 11..15
};
static_assert(std::is_standard_layout_v<RootConstants>);
static_assert(sizeof(RootConstants) == ROOT_CONSTANT_DWORDS * sizeof(std::uint32_t));
static_assert(offsetof(RootConstants, resolution_down_factor) == 4 * sizeof(std::uint32_t));
static_assert(offsetof(RootConstants, base_vertex) == 8 * sizeof(std::uint32_t));
} // namespace Shader::Backend::HLSL
