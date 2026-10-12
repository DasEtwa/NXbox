# SPDX-License-Identifier: GPL-3.0-or-later
"""Executable vertex-fetch and DXIL linkage regressions for test-10 failures."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class VertexBindingTests(unittest.TestCase):
    def test_unaligned_and_truncated_fetch(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++20 compiler unavailable")
        source = r"""
#include <array>
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
using namespace D3D12::Pso;
int main() {
    static_assert(VertexByteSize(VSize::R32G32B32A32) == 16);
    static_assert(VertexByteSize(VSize::R16G16B16) == 6);
    static_assert(VertexByteSize(VSize::R8G8B8) == 3);
    static_assert(VertexAlignment(VSize::R8G8B8A8) == 4);
    static_assert(VertexRequiredBytes(101, 20, 4) == 2024);
    static_assert(VertexRequiredBytes(400, 0, 16) == 16);
    // Attribute at guest offset 14, stride 7, inclusive limit cuts final vertex.
    std::array<std::uint8_t, 19> guest{};
    for (std::size_t i = 0; i < guest.size(); ++i) guest[i] = i + 1;
    std::array<std::uint8_t, 12> packed{};
    RepackVertexAttribute(packed, std::span{guest}.subspan(14), 7, 4);
    assert((packed == std::array<std::uint8_t,12>{15,16,17,18,0,0,0,0,0,0,0,0}));
    RepackVertexAttribute(packed, std::span{guest}.subspan(17), 2, 4);
    assert(packed[0] == 18 && packed[1] == 19 && packed[2] == 0 && packed[4] == 0);
    RepackVertexAttribute(packed, std::span{guest}.subspan(14), 0, 4);
    assert(packed[0] == 15 && packed[4] == 15 && packed[8] == 15);
}
"""
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / "fetch.cpp", Path(temp) / "fetch"
            cpp.write_text(source)
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_runtime_contracts(self):
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        cache = (NATIVE / "d3d12_pipeline_cache.cpp").read_text()
        self.assertIn("draw.params", rasterizer)
        self.assertIn("Pso::RepackVertexAttribute", rasterizer)
        self.assertIn("regs.primitive_restart.index", rasterizer)
        self.assertIn("submitted.num_instances - 1", rasterizer)
        self.assertIn("slot.size - attribute.offset", rasterizer)
        self.assertIn("color_views[previous]->ImageHandle() == view->ImageHandle()", rasterizer)
        self.assertIn("failed_graphics.find(key)", cache)
        self.assertIn("failed_graphics.emplace(key, error.what())", cache)
        self.assertIn("guest_graphics.emplace(request, nullptr)", cache)


class LinkageTests(unittest.TestCase):
    def test_emitter_declares_producer_layout(self):
        source = (ROOT / "src/shader_recompiler/backend/hlsl/hlsl_emit_context.cpp").read_text()
        self.assertIn("? !runtime_info.previous_stage_stores.Generic(i)", source)
        self.assertIn("header += system_inputs", source)

    def test_dxc_preserves_canonical_registers(self):
        dxc = os.environ.get("DXC") or shutil.which("dxc")
        local = Path.home() / "Work/dxc/build/bin/dxc"
        if not dxc and local.is_file():
            dxc = str(local)
        if not dxc:
            self.skipTest("DXC unavailable")
        env = os.environ.copy()
        if local.is_file():
            env["DYLD_LIBRARY_PATH"] = str(local.parent.parent / "lib")
        with tempfile.TemporaryDirectory() as temp:
            hlsl = Path(temp) / "linkage.hlsl"
            hlsl.write_text("""
struct Output { float4 position : SV_Position;
float4 attr0 : TEXCOORD0; float4 attr3 : TEXCOORD3; float4 attr7 : TEXCOORD7; };
struct Input { float4 position : SV_Position;
float4 attr0 : TEXCOORD0; nointerpolation float4 attr3 : TEXCOORD3;
float4 attr7 : TEXCOORD7; bool front_face : SV_IsFrontFace; };
Output vs(uint id : SV_VertexID) {
Output o; o.position=float4(id,0,0,1); o.attr0=1; o.attr3=2; o.attr7=3; return o; }
float4 ps(Input i) : SV_Target { return i.front_face ? i.attr7 + i.attr3 : 0; }
""")
            signatures = []
            for stage in ("vs", "ps"):
                asm = Path(temp) / f"{stage}.txt"
                subprocess.run(
                    [
                        dxc,
                        str(hlsl),
                        "-E",
                        stage,
                        "-T",
                        f"{stage}_6_0",
                        "-Fc",
                        str(asm),
                        "-Fo",
                        str(Path(temp) / f"{stage}.dxil"),
                    ],
                    env=env,
                    check=True,
                    capture_output=True,
                    timeout=30,
                )
                signatures.append(
                    dict(re.findall(r"^; TEXCOORD\s+(\d+)\s+xyzw\s+(\d+)", asm.read_text(), re.M))
                )
            self.assertEqual(signatures[0], {"0": "1", "3": "2", "7": "3"})
            self.assertEqual(signatures[0], signatures[1])


if __name__ == "__main__":
    unittest.main()
