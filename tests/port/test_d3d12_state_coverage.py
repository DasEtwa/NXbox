# SPDX-License-Identifier: GPL-3.0-or-later
"""Maxwell state inventory and executable D3D12 state/conversion contracts.

An inventory entry is not a claim of full rendering support: partial and fallback
entries intentionally expose the remaining shader/API integration work. New Vulkan
register reads fail this test until their handling policy and evidence are reviewed.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"
VULKAN = ROOT / "src/video_core/renderer_vulkan"

# policy, evidence file, executable symbol or explicit rejection reason
STATE_POLICY = {}


def register(policy, filename, evidence, fields):
    for field in fields.split():
        if field in STATE_POLICY:
            raise ValueError(f"Duplicate state policy: {field}")
        STATE_POLICY[field] = (policy, filename, evidence)


register(
    "native",
    "d3d12_pipeline_cache.cpp",
    "FillGraphicsState",
    """
blend_per_target_enabled blend_per_target color_mask_common
polygon_offset_fill_enable polygon_offset_line_enable polygon_offset_point_enable
zeta_enable depth_test_enable depth_test_func depth_write_enabled
stencil_enable stencil_front_op stencil_back_op stencil_front_func_mask
stencil_front_mask stencil_two_side_enable gl_cull_test_enabled gl_front_face
viewport_clip_control vertex_stream_instances
""",
)
register(
    "native",
    "d3d12_rasterizer.cpp",
    "PrepareDraw",
    """
blend_color scissor_test surface_clip viewport_scale_offset_enabled window_origin
stencil_front_ref zpass_pixel_count_enable
""",
)
register(
    "native",
    "d3d12_rasterizer.cpp",
    "ClearOperations::",
    """
clear_color clear_depth clear_stencil clear_surface clear_control viewports
""",
)
register(
    "native",
    "d3d12_pipeline_cache.cpp",
    "s.depth_bias",
    """
depth_bias slope_scale_depth_bias depth_bias_clamp
""",
)
register("native", "d3d12_graphics_pipeline.cpp", "AntialiasedLineEnable", "line_anti_alias_enable")
register(
    "native",
    "d3d12_pipeline_cache.cpp",
    "ConservativeRasterizationTier",
    "conservative_raster_enable",
)
register("partial", "d3d12_pipeline_cache.cpp", "gl_cull_face=FrontAndBack", "gl_cull_face")
register("emulated", "d3d12_rasterizer.cpp", "index_converter.Convert", "primitive_restart")
register("partial", "d3d12_rasterizer.cpp", "IndexConversion::Provoking", "provoking_vertex")
register("partial", "d3d12_pipeline_cache.cpp", "logic_op RTV format", "logic_op")
register("partial", "d3d12_pipeline_cache.cpp", "independent color_mask", "color_mask")
register("partial", "d3d12_pipeline_cache.cpp", "runtime.dual_source_blend", "blend")
register("partial", "d3d12_pipeline_cache.cpp", "alpha_to_one requires", "anti_alias_alpha_control")
register("partial", "d3d12_pipeline_cache.cpp", "s.attributes", "vertex_attrib_format")
register("native", "d3d12_pipeline_cache.cpp", "out.divisor", "vertex_streams")
register("partial", "d3d12_pipeline_cache.cpp", "viewport_transform.swizzle", "viewport_transform")
register("partial", "d3d12_pipeline_cache.cpp", "polygon_mode_front=Point", "polygon_mode_front")
register(
    "partial",
    "d3d12_pipeline_cache.cpp",
    "IndependentFrontAndBackStencilRefMaskSupported",
    """
stencil_back_ref stencil_back_mask stencil_back_func_mask
""",
)
register(
    "shader",
    "d3d12_pipeline_cache.cpp",
    "runtime.alpha_test",
    """
alpha_test_enabled alpha_test_func alpha_test_ref
""",
)
register("shader", "d3d12_pipeline_cache.cpp", "runtime.convert_depth_mode", "depth_mode")
register("shader", "d3d12_pipeline_cache.cpp", "runtime.force_early_z", "mandated_early_z")
register("resource", "d3d12_rasterizer.cpp", "framebuffer->Depth", "rt zeta")
register("resource", "d3d12_texture_cache.cpp", "sample_count", "anti_alias_samples_mode")
register("shader", "d3d12_pipeline_cache.cpp", "state.patch_vertices", "patch_vertices")
register("fallback", "d3d12_pipeline_cache.cpp", "rasterize_enable=0", "rasterize_enable")
register(
    "native",
    "d3d12_pipeline_cache.cpp",
    "DepthBoundsTestSupported",
    "depth_bounds depth_bounds_enable",
)
register("fallback", "d3d12_pipeline_cache.cpp", "iterated_blend requires", "iterated_blend")
register("fallback", "d3d12_pipeline_cache.cpp", "point_size requires", "point_size")
register(
    "fallback",
    "d3d12_pipeline_cache.cpp",
    "line_stipple_enable/line_stipple_params",
    "line_stipple_enable line_stipple_params",
)
register(
    "fallback",
    "d3d12_pipeline_cache.cpp",
    "line_width_smooth/line_width_aliased",
    "line_width_smooth line_width_aliased",
)
register(
    "fallback",
    "d3d12_pipeline_cache.cpp",
    "transform_feedback/stream_out_layout",
    """
transform_feedback transform_feedback_enabled stream_out_layout
""",
)
register("fallback", "d3d12_pipeline_cache.cpp", "tessellation.params", "tessellation")


# Non-register Maxwell draw-manager state and tessellation parameter fields read
# by the same reference files. Keep unsupported DrawTexture visible in coverage.
WORKFLOW_POLICY = {
    **{
        f"draw_state.{field}": ("emulated", "Draw")
        for field in "topology base_index base_instance index_buffer vertex_buffer".split()
    },
    **{
        f"draw_texture_state.{field}": ("fallback", "draw_texture_state requires")
        for field in "dst_x0 dst_x1 dst_y0 dst_y1 src_x0 src_x1 src_y0 src_y1 src_sampler src_texture".split()
    },
    **{
        f"params.{field}": ("fallback", "tessellation.params require")
        for field in "domain_type output_primitives spacing".split()
    },
    **{
        f"params.{field}": ("emulated", "IndirectCount")
        for field in "include_count is_indexed max_draw_counts stride".split()
    },
    "params.is_byte_count": ("fallback", "draw_auto_stride byte-count draw"),
    "params.base_vertex": ("native", "DrawIndexedInstanced"),
    "params.num_vertices": ("native", "DrawInstanced"),
}


def reference_workflow_fields():
    source = "\n".join(
        (VULKAN / filename).read_text()
        for filename in ("fixed_pipeline_state.cpp", "vk_rasterizer.cpp")
    )
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
    return set(re.findall(r"\b(?:draw_state|draw_texture_state|params)\.[A-Za-z_]\w*", source))


def reference_fields():
    fields = set()
    for filename in ("fixed_pipeline_state.cpp", "vk_rasterizer.cpp", "vk_state_tracker.cpp"):
        source = (VULKAN / filename).read_text()
        source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
        fields.update(re.findall(r"\bregs\.([A-Za-z_]\w*)", source))
        fields.update(re.findall(r"\bOFF\((\w+)", source))
    return fields - {"field_name", "IsShaderConfigEnabled"}


DRIVER = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>
#include "shader_recompiler/backend/hlsl/hlsl_root_constants.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_index_converter.h"
using namespace D3D12;
using namespace IndexConversion;
std::vector<unsigned> expand(Topology topology, unsigned n, Provoking mode,
                             Restart restart = {}) {
    std::vector<std::byte> input(n * 4);
    for (unsigned i = 0; i < n; ++i) input[i*4] = std::byte(i);
    const auto layout = Analyze(topology, Format::Uint32, input, restart);
    std::vector<std::byte> output(layout.bytes);
    Convert(topology, Format::Uint32, input, output, restart, mode);
    std::vector<unsigned> result;
    for (unsigned i = 0; i < layout.count; ++i)
        result.push_back(std::to_integer<unsigned>(output[i*4]));
    return result;
}
unsigned sdk_logic(unsigned op, unsigned s, unsigned d) {
    switch(op) {
    case 0: return 0; case 1: return 255; case 2: return s;
    case 3: return ~s & 255; case 4: return d; case 5: return ~d & 255;
    case 6: return s & d; case 7: return ~(s & d) & 255; case 8: return s | d;
    case 9: return ~(s | d) & 255; case 10: return s ^ d;
    case 11: return ~(s ^ d) & 255; case 12: return s & ~d;
    case 13: return ~s & d; case 14: return (s | ~d) & 255;
    case 15: return (~s | d) & 255;
    }
    assert(false); return 0;
}
int main() {
    static_assert(Pso::DsvHasStencil(20) && Pso::DsvHasStencil(45));
    static_assert(!Pso::DsvHasStencil(40) && !Pso::DsvHasStencil(55) && !Pso::DsvHasStencil(0));
    static_assert(offsetof(Shader::Backend::HLSL::RootConstants, draw_id) == 10 * sizeof(std::uint32_t));
    static_assert(!Pso::IndependentStencil(false,1,1,2,255,0,255,0));
    static_assert(!Pso::IndependentStencil(true,2,1,2,255,0,255,0));
    static_assert(!Pso::IndependentStencil(true,3,1,2,255,0,255,0));
    static_assert(!Pso::IndependentStencil(true,1,0x101,1,0x1ff,255,255,255));
    static_assert(Pso::IndependentStencil(true,1,1,2,255,255,255,255));
    static_assert(Pso::IndependentStencil(true,1,1,1,255,127,255,255));
    static_assert(Pso::IndependentStencil(true,1,1,1,255,255,255,127));
    static_assert(Pso::kBlendAlphaFactor == 20 && Pso::kBlendInvAlphaFactor == 21);
    static_assert(Pso::AlphaBlendFactor(3) == 5 && Pso::AlphaBlendFactor(4) == 6);
    static_assert(Pso::AlphaBlendFactor(9) == 7 && Pso::AlphaBlendFactor(10) == 8);
    static_assert(Pso::AlphaBlendFactor(11) == 2);
    static_assert(Pso::AlphaBlendFactor(16) == 18 && Pso::AlphaBlendFactor(17) == 19);
    static_assert(Pso::AlphaBlendFactor(0xc003) == 14);
    // Independent GL truth-table oracle: 16 operations * every pair of 8-bit values.
    for (unsigned s=0;s<256;++s) for (unsigned d=0;d<256;++d) {
        const std::array<unsigned,16> gl{0,s&d,s&~d,s,~s&d,d,s^d,s|d,
            ~(s|d)&255,~(s^d)&255,~d&255,(s|~d)&255,~s&255,(~s|d)&255,
            ~(s&d)&255,255};
        for(unsigned op=0;op<16;++op)
            assert(sdk_logic(Pso::LogicOp(0x1500+op),s,d)==gl[op]);
    }
    // Stride/count bounds checked independently by walking the actual commands.
    for (unsigned bytes=0;bytes<128;++bytes) for(unsigned stride=0;stride<40;++stride)
      for(unsigned count=0;count<9;++count) {
        unsigned expected=0, step=stride?stride:20;
        if(step>=20) for(unsigned at=0;at+20<=bytes && expected<7 && expected<count;at+=step)
            ++expected;
        assert(Pso::IndirectCount(bytes,stride,20,7,count)==expected);
      }
    assert(Pso::IndirectCount(UINT64_MAX,UINT64_MAX,20,UINT64_MAX,UINT64_MAX)==1);
    assert(Pso::IndirectCount(UINT64_MAX,0,0,UINT64_MAX,UINT64_MAX)==0);
    assert((expand(Topology::Triangles,6,Provoking::Last)==std::vector<unsigned>{2,0,1,5,3,4}));
    assert((expand(Topology::Lines,4,Provoking::Last)==std::vector<unsigned>{1,0,3,2}));
    assert((expand(Topology::TriangleStrip,5,Provoking::First)==std::vector<unsigned>{0,1,2,1,3,2,2,3,4}));
    assert((expand(Topology::TriangleStrip,5,Provoking::Last)==std::vector<unsigned>{2,0,1,3,2,1,4,2,3}));
    assert((expand(Topology::TriangleFan,5,Provoking::First)==std::vector<unsigned>{1,2,0,2,3,0,3,4,0}));
    assert((expand(Topology::Polygon,5,Provoking::First)==std::vector<unsigned>{1,2,0,2,3,0,3,4,0}));
    assert((expand(Topology::TriangleFan,5,Provoking::Last)==std::vector<unsigned>{2,0,1,3,0,2,4,0,3}));
    assert((expand(Topology::QuadStrip,4,Provoking::Last)==std::vector<unsigned>{3,1,0,3,0,2}));
    // New adjacency list/strip conversion discards incomplete restart segments.
    assert((expand(LinesAdjacency,7,Provoking::Preserve)==std::vector<unsigned>{0,1,2,3}));
    assert((expand(LineStripAdjacency,5,Provoking::Preserve)==std::vector<unsigned>{0,1,2,3,1,2,3,4}));
    assert((expand(TrianglesAdjacency,8,Provoking::Preserve)==std::vector<unsigned>{0,1,2,3,4,5}));
    assert((expand(TriangleStripAdjacency,6,Provoking::Preserve)==std::vector<unsigned>{0,1,2,5,4,3}));
    assert((expand(TriangleStripAdjacency,8,Provoking::Preserve)==std::vector<unsigned>{0,1,2,6,4,3,2,5,6,7,4,0}));
    assert((expand(TriangleStripAdjacency,10,Provoking::Preserve)==std::vector<unsigned>{0,1,2,6,4,3,2,5,6,8,4,0,4,2,6,9,8,7}));
    assert((expand(TriangleStripAdjacency,12,Provoking::Preserve)==std::vector<unsigned>{0,1,2,6,4,3,2,5,6,8,4,0,4,2,6,10,8,7,6,9,10,11,8,4}));
    for (auto topology : {LinesAdjacency,LineStripAdjacency,TrianglesAdjacency,TriangleStripAdjacency}) {
        for (unsigned n=0;n<16;++n) for(unsigned marker=0;marker<n;++marker) {
            const auto all=expand(topology,n,Provoking::Preserve,{true,marker});
            for(auto value:all) assert(value!=marker);
        }
    }
    // Constant alpha can use the legacy blend factor when RGB does not also
    // need constant color. Unbound/disabled targets must not cause rejection.
    Pso::GraphicsPsoState constant{};
    constant.blend[7].src = Pso::kBlendAlphaFactor;
    constant.blend[7].enable = 1;
    assert(Pso::ConstantBlendFactors(constant) == 0);
    constant.rtv_formats[7] = 28;
    assert(Pso::ConstantBlendFactors(constant) == 2);
    constant.blend[7].dst = Pso::kBlendFactor;
    assert(Pso::ConstantBlendFactors(constant) == 3);
    constant.blend[7].enable = 0;
    assert(Pso::ConstantBlendFactors(constant) == 0);
    constant.blend[7].enable = 1;
    constant.blend[7].dst = Pso::kBlendInvAlphaFactor;
    Pso::EmulateConstantAlpha(constant);
    assert(constant.blend[7].src == 14 && constant.blend[7].dst == 15);
    assert(Pso::ConstantBlendFactors(constant) == 1);
    // Every newly keyed native state changes the PSO hash; full-width divisors survive.
    Pso::GraphicsPsoState state{};
    auto hash=Pso::HashBytes(&state,sizeof(state));
    for(auto field:{&state.logic_enable,&state.logic_op,&state.alpha_to_coverage,
                    &state.conservative_raster,&state.smooth_lines,&state.depth_bounds,
                    &state.stencil_back_read_mask,&state.stencil_back_write_mask}) {
        *field=1; assert(hash!=Pso::HashBytes(&state,sizeof(state))); *field=0;
    }
    state.attributes[0].divisor=UINT32_MAX;
    assert(hash!=Pso::HashBytes(&state,sizeof(state)));
    // Same topology class/shaders still need different IA wrapper identities.
    const std::uint32_t line_list=2, line_strip=3;
    assert(Pso::HashBytes(&line_list,sizeof(line_list),123) !=
           Pso::HashBytes(&line_strip,sizeof(line_strip),123));
}
"""


class D3D12StateCoverageTests(unittest.TestCase):
    def test_vulkan_register_inventory_has_explicit_policy(self):
        self.assertEqual(reference_fields(), set(STATE_POLICY))
        for field, (policy, filename, evidence) in STATE_POLICY.items():
            with self.subTest(field=field, policy=policy):
                self.assertTrue(
                    evidence in (NATIVE / filename).read_text(),
                    f"Missing evidence: {filename}: {evidence}",
                )
        # Inventory coverage includes explicit skips; never equate it with semantic coverage.
        self.assertEqual(len(STATE_POLICY), 74)
        self.assertEqual(reference_workflow_fields(), set(WORKFLOW_POLICY))
        self.assertEqual(len(WORKFLOW_POLICY), 25)
        source = (NATIVE / "d3d12_pipeline_cache.cpp").read_text() + (
            NATIVE / "d3d12_rasterizer.cpp"
        ).read_text()
        for field, (policy, evidence) in WORKFLOW_POLICY.items():
            with self.subTest(field=field, policy=policy):
                self.assertTrue(evidence in source, f"Missing workflow handling: {field}")

    def test_fixed_state_and_conversion_semantics(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-state-") as temp:
            source = Path(temp) / "state.cpp"
            binary = Path(temp) / "state"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    str(NATIVE / "d3d12_index_converter.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_last_quad_fallback_only_rejects_loaded_flat_varyings(self):
        source = (NATIVE / "d3d12_pipeline_cache.cpp").read_text()
        guard = source.split("const char* UnsupportedState", 1)[1].split("u8 StencilFaceMask", 1)[0]
        self.assertNotIn("provoking_vertex=Last", guard)
        self.assertIn("program.info.loads.Generic(attribute)", source)
        self.assertIn(
            "program.info.interpolation[attribute] == Shader::Interpolation::Flat", source
        )
        self.assertIn("sizeof(regs.provoking_vertex)", source)
        graphics = source.split("GraphicsPipeline& PipelineCache::GetGraphics", 1)[1].split(
            "ComputePipeline& PipelineCache::GetCompute", 1
        )[0]
        self.assertNotIn("key.stage_hashes[5] = d3d_topology", graphics)
        self.assertGreater(
            graphics.index("Pso::HashBytes(&d3d_topology"), graphics.index("total += stage.counts")
        )

    def test_guest_shader_failures_skip_instead_of_rendering_stub_outputs(self):
        source = (NATIVE / "d3d12_pipeline_cache.cpp").read_text()
        self.assertNotIn("StubHlsl", source)
        self.assertIn("reported.emplace(reason).second", source)
        self.assertIn("action=skip_draw", source)
        self.assertIn("requires vertex pulling/component conversion", source)
        self.assertIn("vertex_reads & (u32{1} << index)", source)
        self.assertIn("attribute.swap_rb || attribute.scaled", source)
        self.assertIn("catch (...) ", source)

    def test_indirect_reuses_draw_with_coherent_arguments_and_restores_state(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        indirect = source.split("void RasterizerD3D12::DrawIndirect()", 1)[1].split(
            "bool RasterizerD3D12::AccelerateConditionalRendering", 1
        )[0]
        for evidence in (
            "DownloadMemory",
            "params.count_start_address",
            "IndirectCount",
            "state = saved_state",
            "command.StartInstanceLocation",
            "command.BaseVertexLocation",
            "indirect_draw_id = static_cast<u32>(i)",
            "indirect_draw_id = saved_draw_id",
            "Draw(true",
            "Draw(false",
        ):
            self.assertIn(evidence, indirect)
        self.assertNotIn("ExecuteIndirect", indirect)


if __name__ == "__main__":
    unittest.main()
