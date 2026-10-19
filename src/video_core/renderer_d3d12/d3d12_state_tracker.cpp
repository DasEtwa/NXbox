// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_state_tracker.h"

#include "common/assert.h"

namespace D3D12 {
namespace {
using Maxwell3D = Tegra::Engines::Maxwell3D;

void Register(Maxwell3D::DirtyState::Tables& tables, size_t first, size_t count) {
    for (size_t offset = first; offset < first + count; ++offset) {
        // Preserve every common flag. No registered word may need a third table entry.
        if (tables[0][offset] == Dirty::Graphics || tables[1][offset] == Dirty::Graphics)
            continue;
        if (tables[0][offset] == VideoCommon::Dirty::NullEntry)
            tables[0][offset] = Dirty::Graphics;
        else {
            ASSERT_MSG(tables[1][offset] == VideoCommon::Dirty::NullEntry,
                       "D3D12 dirty table collision at method={:x} flags={},{}", offset,
                       tables[0][offset], tables[1][offset]);
            tables[1][offset] = Dirty::Graphics;
        }
    }
}
} // namespace

void StateTracker::SetupDirtyFlags(Maxwell3D::DirtyState::Tables& tables) {
    VideoCommon::Dirty::SetupDirtyFlags(tables);
#define TRACK(field)                                                                               \
    Register(tables, MAXWELL3D_REG_INDEX(field), sizeof(Maxwell3D::Regs::field) / (sizeof(u32)))
    TRACK(alpha_test_enabled);
    TRACK(alpha_test_func);
    TRACK(alpha_test_ref);
    TRACK(anti_alias_alpha_control);
    TRACK(blend);
    TRACK(blend_color);
    TRACK(blend_per_target);
    TRACK(blend_per_target_enabled);
    TRACK(color_mask);
    TRACK(color_mask_common);
    TRACK(conservative_raster_enable);
    TRACK(depth_bias);
    TRACK(depth_bias_clamp);
    TRACK(depth_bounds);
    TRACK(depth_bounds_enable);
    TRACK(depth_mode);
    TRACK(depth_test_enable);
    TRACK(depth_test_func);
    TRACK(depth_write_enabled);
    TRACK(gl_cull_face);
    TRACK(gl_cull_test_enabled);
    TRACK(gl_front_face);
    TRACK(iterated_blend);
    TRACK(line_anti_alias_enable);
    TRACK(line_stipple_enable);
    TRACK(line_width_aliased);
    TRACK(line_width_smooth);
    TRACK(logic_op);
    TRACK(mandated_early_z);
    TRACK(patch_vertices);
    TRACK(point_size);
    TRACK(polygon_mode_back);
    TRACK(polygon_mode_front);
    TRACK(polygon_offset_fill_enable);
    TRACK(polygon_offset_line_enable);
    TRACK(polygon_offset_point_enable);
    TRACK(provoking_vertex);
    TRACK(rasterize_enable);
    TRACK(scissor_test);
    TRACK(slope_scale_depth_bias);
    TRACK(stencil_back_func_mask);
    TRACK(stencil_back_mask);
    TRACK(stencil_back_op);
    TRACK(stencil_back_ref);
    TRACK(stencil_enable);
    TRACK(stencil_front_func_mask);
    TRACK(stencil_front_mask);
    TRACK(stencil_front_op);
    TRACK(stencil_front_ref);
    TRACK(stencil_two_side_enable);
    TRACK(stream_out_layout);
    TRACK(surface_clip);
    TRACK(transform_feedback_enabled);
    TRACK(vertex_attrib_format);
    TRACK(vertex_stream_instances);
    TRACK(viewport_clip_control);
    TRACK(viewport_scale_offset_enabled);
    TRACK(viewport_transform);
    TRACK(window_origin);
    TRACK(transform_feedback.controls);
    for (size_t i = 0; i < Maxwell3D::Regs::NumVertexArrays; ++i) {
        Register(tables,
                 MAXWELL3D_REG_INDEX(vertex_streams) +
                     i * sizeof(Maxwell3D::Regs::VertexStream) / sizeof(u32) +
                     offsetof(Maxwell3D::Regs::VertexStream, frequency) / sizeof(u32),
                 1);
    }
    // zeta_enable and tessellation pipeline enables already have two common flags.
    // GraphicsStateInputs compares their values without replacing either common flag.
#undef TRACK
}

void StateTracker::Bind(Maxwell3D& maxwell) {
    maxwell.dirty.flags[Dirty::Graphics] = true;
}

u64 StateTracker::Update(Maxwell3D& maxwell) {
    auto& flags = maxwell.dirty.flags;
    if (invalidated) {
        flags[Dirty::Graphics] = true;
        invalidated = false;
    }
    // These two common bits belong to the native viewport/scissor consumer. Neither shared
    // buffer/texture/shader flags nor their table entries are consumed here.
    const bool rescale =
        flags[VideoCommon::Dirty::RescaleViewports] || flags[VideoCommon::Dirty::RescaleScissors];
    flags[VideoCommon::Dirty::RescaleViewports] = false;
    flags[VideoCommon::Dirty::RescaleScissors] = false;
    if (flags[Dirty::Graphics] || rescale) {
        ++generation;
        flags[Dirty::Graphics] = false;
    }
    return generation;
}
} // namespace D3D12
