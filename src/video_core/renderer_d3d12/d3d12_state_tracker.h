// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <limits>
#include <string_view>

#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_state_cache.h"

namespace D3D12 {
namespace Dirty {
enum : u8 {
    First = VideoCommon::Dirty::LastCommonEntry,
    Graphics,
    Last,
};
static_assert(Last <= (std::numeric_limits<u8>::max)());
} // namespace Dirty

// GPU-thread only. One conservative flag covers overlapping pipeline/dynamic dependencies.
// Consuming it advances a generation, so independent caches cannot consume each other's work.
class StateTracker {
public:
    static void SetupDirtyFlags(Tegra::Engines::Maxwell3D::DirtyState::Tables& tables);
    static void Bind(Tegra::Engines::Maxwell3D& maxwell);

    void Invalidate() noexcept {
        // Defer setting flags until an engine is available; never retain a channel pointer.
        invalidated = true;
    }
    u64 Update(Tegra::Engines::Maxwell3D& maxwell);
    void SetRenderArea(u32 width, u32 height) noexcept {
        if (width != render_width || height != render_height) {
            render_width = width;
            render_height = height;
            Invalidate();
        }
    }

private:
    u64 generation{};
    u32 render_width{}, render_height{};
    bool invalidated{true};
};

bool StateFastpathEnabled() noexcept;
bool StateVerifyEnabled() noexcept;
// Call only in verify mode. Fixed, bounded GPU-thread diagnostic storage.
void StateVerifyMismatch(std::string_view what, const void* fast, const void* slow, size_t size);

} // namespace D3D12
