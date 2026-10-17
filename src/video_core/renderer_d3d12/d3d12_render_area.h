// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace D3D12 {
struct AttachmentExtent {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t layers;
    constexpr bool operator==(const AttachmentExtent &) const = default;
};
constexpr AttachmentExtent IntersectAttachments(AttachmentExtent a, AttachmentExtent b) {
    return {std::min(a.width, b.width), std::min(a.height, b.height), std::min(a.layers, b.layers)};
}
// Keep depth/stencil in its original resource. Colors can be copied to matching
// temporaries; a scissor and copyback limit writes to the common render area.
constexpr AttachmentExtent MatchingAttachmentExtent(AttachmentExtent area, AttachmentExtent depth,
                                                    bool has_depth) {
    return has_depth ? AttachmentExtent{depth.width, depth.height, area.layers} : area;
}
} // namespace D3D12
