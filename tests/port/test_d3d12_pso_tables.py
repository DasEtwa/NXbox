# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the std-only Maxwell -> D3D12 PSO tables (no Windows SDK needed)."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
using namespace D3D12::Pso;
int main() {
    // Blend factors: D3D numbering, GL numbering, constants and dual source.
    static_assert(BlendFactor(0x1) == 1 && BlendFactor(0xB) == 11);
    static_assert(BlendFactor(0x4000) == 1 && BlendFactor(0x4001) == 2);
    static_assert(BlendFactor(0x4302) == 5 && BlendFactor(0x4303) == 6);
    static_assert(BlendFactor(0x4308) == 11);
    static_assert(BlendFactor(0xC001) == kBlendFactor && BlendFactor(0xC002) == kBlendInvFactor);
    static_assert(BlendFactor(0xC003) == kBlendAlphaFactor);
    static_assert(BlendFactor(0xC900) == 16 && BlendFactor(0xC903) == 19);
    static_assert(BlendFactor(0x10) == 16 && BlendFactor(0x13) == 19);
    static_assert(BlendFactor(0xC) == 5 && BlendFactor(0xD) == 6);
    static_assert(BlendFactor(0xDEAD) == kBlendOne);
    // Equations
    static_assert(BlendOp(0x8006) == 1 && BlendOp(0x800A) == 2 && BlendOp(0x800B) == 3);
    static_assert(BlendOp(0x8007) == 4 && BlendOp(0x8008) == 5 && BlendOp(3) == 3);
    // Comparison + stencil ops
    for (unsigned i = 0; i < 8; ++i) {
        assert(ComparisonFunc(1 + i) == 1 + i);
        assert(ComparisonFunc(0x200 + i) == 1 + i);
    }
    static_assert(StencilOp(0x1E00) == 1 && StencilOp(0) == 2 && StencilOp(0x150A) == 6);
    static_assert(StencilOp(0x8507) == 7 && StencilOp(0x8508) == 8 && StencilOp(5) == 5);
    // Cull
    static_assert(CullMode(false, 0x405).mode == kCullNone);
    static_assert(CullMode(true, 0x404).mode == kCullFront);
    static_assert(CullMode(true, 0x405).mode == kCullBack);
    static_assert(CullMode(true, 0x408).cull_all);
    // Topology
    static_assert(TopologyType(0) == kTopologyTypePoint && TopologyType(3) == kTopologyTypeLine);
    static_assert(TopologyType(5) == kTopologyTypeTriangle && TopologyType(0xE) == kTopologyTypePatch);
    static_assert(PrimitiveTopology(0x7).needs_conversion && !PrimitiveTopology(0x4).needs_conversion);
    static_assert(PrimitiveTopology(0xE, 4).topology == 36);
    // Vertex formats
    static_assert(VertexFormatFor(VSize::R32G32B32A32, VType::Float).dxgi == 2);
    static_assert(VertexFormatFor(VSize::R32G32B32, VType::Float).dxgi == 6);
    static_assert(VertexFormatFor(VSize::R16G16, VType::SNorm).dxgi == 37);
    static_assert(VertexFormatFor(VSize::R16G16B16, VType::Float).dxgi == 10);
    static_assert(VertexFormatFor(VSize::R8G8B8A8, VType::UNorm).dxgi == 28);
    static_assert(VertexFormatFor(VSize::R8G8B8A8, VType::UNorm, true).dxgi == 87);
    static_assert(VertexFormatFor(VSize::R8G8B8A8, VType::UInt, true).swap_rb);
    static_assert(VertexFormatFor(VSize::R8G8B8A8, VType::UScaled).dxgi == 30);
    static_assert(VertexFormatFor(VSize::R8G8B8A8, VType::UScaled).scaled);
    static_assert(VertexFormatFor(VSize::A2B10G10R10, VType::UNorm).dxgi == 24);
    // Signed 10:10:10:2 (SM3DW/MK8D normals and tangents) fetches as R10G10B10A2_UINT and
    // decodes in HLSL; it must not be rejected (that skipped every level-geometry draw).
    static_assert(VertexFormatFor(VSize::A2B10G10R10, VType::SNorm).dxgi == 25);
    static_assert(VertexFormatFor(VSize::A2B10G10R10, VType::SInt).dxgi == 25);
    static_assert(!VertexFormatFor(VSize::A2B10G10R10, VType::SNorm).scaled);
    static_assert(VertexFormatForInputAssembler(VSize::A2B10G10R10, VType::SNorm).dxgi == 25);
    static_assert(VertexFormatFor(VSize::A2B10G10R10, VType::SScaled).dxgi == 0);
    static_assert(VertexSignedPacked10(VSize::A2B10G10R10, VType::SNorm));
    static_assert(VertexSignedPacked10(VSize::A2B10G10R10, VType::SInt));
    static_assert(!VertexSignedPacked10(VSize::A2B10G10R10, VType::UNorm));
    static_assert(!VertexSignedPacked10(VSize::R16G16B16A16, VType::SNorm));
    static_assert(VertexFormatFor(VSize::B10G11R11, VType::Float).dxgi == 26);
    static_assert(VertexFormatFor(VSize::R32, VType::UInt).dxgi == 42);
    static_assert(VertexFormatFor(VSize::R8, VType::SInt).dxgi == 64);
    static_assert(VertexFormatFor(0, VType::Float).dxgi == 0);
    static_assert(DsvFormat(39) == 40 && DsvFormat(44) == 45 && DsvFormat(0) == 0);
    // State key hashing is deterministic and sensitive to every field.
    GraphicsPsoState a{}, b{};
    assert(HashBytes(&a, sizeof a) == HashBytes(&b, sizeof b));
    b.blend[7].dst = 1;
    assert(HashBytes(&a, sizeof a) != HashBytes(&b, sizeof b));
}
"""


class PsoTablesTests(unittest.TestCase):
    def test_tables(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-pso-") as temp:
            source = Path(temp) / "pso.cpp"
            binary = Path(temp) / "pso"
            source.write_text(DRIVER)
            subprocess.run(
                ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "src"), str(source), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)
