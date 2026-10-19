// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

#include "video_core/renderer_d3d12/d3d12_pso_tables.h"

namespace D3D12 {

// Compare fields, never padding. These inputs are not covered by register-write flags.
struct GraphicsStateInputs {
    std::uint64_t generation{};
    std::array<std::uint64_t, 6> hashes{};
    Pso::FramebufferFormats formats{};
    std::uint32_t topology{};
    std::uint32_t zeta_enable{};
    bool indexed{};
    std::array<std::uint32_t, 2> tessellation_enables{};

    bool operator==(const GraphicsStateInputs& other) const noexcept {
        return generation == other.generation && hashes == other.hashes &&
               formats.colors == other.formats.colors && formats.depth == other.formats.depth &&
               formats.sample_count == other.formats.sample_count && topology == other.topology &&
               zeta_enable == other.zeta_enable && indexed == other.indexed &&
               tessellation_enables == other.tessellation_enables;
    }
};

struct DynamicStateInputs {
    GraphicsStateInputs graphics{};
    std::uint32_t width{}, height{}, converted_topology{};
    const void* pipeline{};
    bool operator==(const DynamicStateInputs&) const = default;
};

// Fixed storage, no ownership, allocations or locks. A failed/unsupported computation must
// not commit its inputs: a later draw must retry it just like the uncached path.
template <typename Inputs>
class StateInputCache {
public:
    bool Matches(const Inputs& inputs) const noexcept {
        return valid && previous == inputs;
    }
    void Store(const Inputs& inputs) noexcept {
        previous = inputs;
        valid = true;
    }
    void Invalidate() noexcept {
        valid = false;
    }

private:
    Inputs previous{};
    bool valid{};
};

} // namespace D3D12
