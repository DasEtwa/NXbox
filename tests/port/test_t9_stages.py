# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Executable vertex-fetch and DXIL linkage regressions for test-10 failures."""

import os
import re
import shutil
import shlex
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
    // A 64-vertex fan expands to 186 indices, but still fetches vertices 11..74.
    static_assert(VertexLastNonIndexed(11, 64) == 74);
    static_assert(VertexRequiredBytes(VertexLastNonIndexed(11, 64), 16, 16) == 1200);
    static_assert(VertexLastNonIndexed(0xffffffff, 2) == 0x100000000ull);
    static_assert(VertexRequiredBytes(400, 0, 16) == 16);
    static_assert(RenderTargetLayersOverlap(1, 2, 2, 3));
    static_assert(!RenderTargetLayersOverlap(1, 1, 2, 1));
    static_assert(!RenderTargetLayersOverlap(0, 0, 0, 2));
    static_assert(RenderTargetLayersOverlap(0xffffffff, 2, 0xffffffff, 1));
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

    def test_vertex_component_conversion(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++20 compiler unavailable")
        source = r"""
#include <array>
#include <cassert>
#include <cstring>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
using namespace D3D12::Pso;
int main() {
    static_assert(VertexFormatForInputAssembler(VSize::A8, VType::SNorm).dxgi == 63);
    static_assert(VertexFormatForInputAssembler(VSize::R8G8B8A8, VType::UNorm, true).dxgi == 28);
    static_assert(VertexFormatForInputAssembler(VSize::B10G11R11, VType::Float, true).dxgi == 0);
    std::array<std::uint8_t, 6> guest{1,2,3,4,5,6};
    std::array<std::uint8_t, 8> out{};
    RepackVertexFormat(out, guest, 3, VSize::R8G8B8, VType::UNorm, true);
    assert((out == std::array<std::uint8_t,8>{3,2,1,255,6,5,4,255}));
    RepackVertexFormat(out, guest, 6, VSize::R16G16B16, VType::Float, false);
    assert(out[0]==1 && out[5]==6 && out[6]==0 && out[7]==0x3c);
    RepackVertexFormat(out, guest, 1, VSize::A8, VType::UInt, false);
    assert(out[0]==1 && out[3]==4 && out[6]==0 && out[7]==0);
    RepackVertexFormat(out, guest, 2, VSize::G8R8, VType::UNorm, false);
    assert(out[0]==1 && out[1]==2 && out[2]==3 && out[3]==4);
    RepackVertexFormat(out, guest, 4, VSize::X8B8G8R8, VType::UNorm, false);
    assert(out[0]==1 && out[3]==4);
    RepackVertexFormat(out, guest, 1, VSize::R8, VType::UInt, true);
    assert(out[0]==0 && out[1]==0 && out[2]==1 && out[3]==1);
    std::uint32_t bits = 9u | (17u << 10) | (25u << 20) | (3u << 30);
    std::array<std::uint8_t,4> packed{};
    std::memcpy(packed.data(), &bits, 4);
    RepackVertexFormat(packed, std::span{guest}.first(0), 4, VSize::A2B10G10R10,
                       VType::UNorm, true);
    assert((packed == std::array<std::uint8_t,4>{}));
    std::array<std::uint8_t,4> input{};
    std::memcpy(input.data(), &bits, 4);
    RepackVertexFormat(packed, input, 4, VSize::A2B10G10R10, VType::UNorm, true);
    std::uint32_t result{}; std::memcpy(&result, packed.data(), 4);
    assert(result == (25u | (17u << 10) | (9u << 20) | (3u << 30)));
}
"""
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / "conversion.cpp", Path(temp) / "conversion"
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

    def test_input_format_inventory(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++20 compiler unavailable")
        maxwell = (ROOT / "src/video_core/engines/maxwell_3d.h").read_text()
        enum = maxwell.split("enum class Size : u32 {", 1)[1].split("};", 1)[0]
        sizes = [int(value, 16) for value in re.findall(r"Size_\w+ = (0x\w+)", enum)]
        self.assertEqual(len(sizes), 17, "review newly added Maxwell vertex sizes")
        source = r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
int main() {
    using namespace D3D12::Pso;
    const unsigned sizes[] = {SIZES};
    unsigned baseline=0, mapped=0, native=0;
    for (auto size:sizes) for(unsigned type=1;type<=7;++type) for(bool bgra:{false,true}) {
        const auto old = VertexFormatFor(size,type,bgra);
        const auto current = VertexFormatForInputAssembler(size,type,bgra);
        mapped += old.dxgi != 0;
        baseline += old.dxgi && !old.swap_rb && !old.scaled &&
                    size != VSize::R16G16B16 && size != VSize::R8G8B8;
        native += current.dxgi != 0;
        if (current.dxgi) {
            assert(current.dxgi != 87 && current.dxgi != 65);
            assert(VertexStorageByteSize(size,current.swap_rb) >= VertexByteSize(size));
        }
    }
    assert(baseline==56 && mapped==186 && native==191);
}
""".replace("SIZES", ",".join(map(str, sizes)))
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / "inventory.cpp", Path(temp) / "inventory"
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
        self.assertIn("Pso::VertexLastNonIndexed", rasterizer)
        self.assertIn("Pso::RepackVertexFormat", rasterizer)
        self.assertIn("regs.primitive_restart.index", rasterizer)
        self.assertIn("submitted.num_instances - 1", rasterizer)
        self.assertIn("slot.size - attribute.offset", rasterizer)
        self.assertIn("Pso::RenderTargetLayersOverlap", rasterizer)
        self.assertIn("failed_graphics.find(key)", cache)
        self.assertIn("failed_graphics.emplace(key, error.what())", cache)
        self.assertIn("StoreGuestGraphics(request, nullptr)", cache)


class LinkageTests(unittest.TestCase):
    def test_emitter_declares_producer_layout(self):
        source = (ROOT / "src/shader_recompiler/backend/hlsl/hlsl_emit_context.cpp").read_text()
        self.assertIn("? !runtime_info.previous_stage_stores.Generic(i)", source)
        self.assertIn("header += system_inputs", source)
        self.assertIn("input.vertex_id + nx_base_vertex", source)
        self.assertIn("input.instance_id + nx_base_instance", source)

    def test_real_emitter_geometry_and_stream_output(self):
        build = Path(os.environ.get("NXBOX_T9_HLSL_BUILD", "/tmp/nxbox-t9-corpus-build"))
        objects = build / "CMakeFiles/nxbox-shader-corpus.dir"
        flags_file = objects / "flags.make"
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler or not flags_file.is_file():
            self.skipTest("set NXBOX_T9_HLSL_BUILD to a built corpus harness")
        flags = flags_file.read_text()
        includes = shlex.split(re.search(r"^CXX_INCLUDES = (.*)$", flags, re.M)[1])
        fixture = r"""
#include <filesystem>
#include <fstream>
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
using namespace Shader;
int main(int argc, char** argv) {
    if(argc != 2) return 1;
    Profile profile{}; profile.support_int64 = true;
    IR::Program previous;
    for (auto stage : {Stage::VertexB, Stage::Geometry, Stage::Fragment}) {
        ObjectPool<IR::Inst> instructions;
        ObjectPool<IR::Block> blocks;
        IR::Program p; p.stage = stage;
        p.output_vertices = 3; p.invocations = 1;
        p.output_topology = OutputTopology::TriangleStrip;
        auto* b = blocks.Create(instructions);
        p.blocks = {b}; p.post_order_blocks = {b};
        p.syntax_list.push_back({.data = {.block = b}, .type = IR::AbstractSyntaxNode::Type::Block});
        p.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        RuntimeInfo runtime{}; runtime.input_topology = InputTopology::Triangles;
        runtime.previous_stage_stores = previous.info.stores;
        runtime.generic_input_types[0] = AttributeType::SignedScaled;
        IR::IREmitter ir{*b}; ir.Prologue();
        if(stage == Stage::VertexB) {
            p.info.loads.Set(IR::Attribute::Generic0X);
            ir.SetAttribute(IR::Attribute::PositionX, ir.GetAttribute(IR::Attribute::Generic0X), ir.Imm32(0u));
            for(auto attr : {IR::Attribute::Generic0X, IR::Attribute::Generic3X, IR::Attribute::Generic7X}) {
                p.info.stores.Set(attr);
                ir.SetAttribute(attr, ir.Imm32(2.0f), ir.Imm32(0u));
            }
        } else if(stage == Stage::Geometry) {
            runtime.y_negate = true; runtime.convert_depth_mode = true;
            runtime.xfb_count = 32;
            p.info.loads.Set(IR::Attribute::Generic7X);
            p.info.stores.Set(IR::Attribute::Generic7X);
            ir.SetAttribute(IR::Attribute::Generic7X,
                            ir.GetAttribute(IR::Attribute::Generic7X, ir.Imm32(1u)), ir.Imm32(0u));
            ir.EmitVertex(ir.Imm32(0u)); ir.EndPrimitive(ir.Imm32(0u));
        } else {
            p.info.loads.Set(IR::Attribute::Generic7X);
            p.info.stores_frag_color[0] = true;
            ir.SetFragColor(0, 0, ir.GetAttribute(IR::Attribute::Generic7X));
        }
        ir.Epilogue();
        Backend::Bindings bindings{};
        const auto source = Backend::HLSL::EmitHLSL(profile, runtime, p, bindings);
        const auto name = stage == Stage::VertexB ? "vs" : stage == Stage::Geometry ? "gs" : "ps";
        std::ofstream(std::filesystem::path(argv[1]) / (std::string(name) + ".hlsl")) << source;
        previous.info = p.info;
    }
}
"""
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / "emit.cpp", Path(temp) / "emit"
            cpp.write_text(fixture)
            compiled = [str(p) for p in objects.rglob("*.o") if p.name != "main.cpp.o"]
            fmt = next(build.rglob("libfmt.a"), None)
            if not fmt:
                self.skipTest("corpus fmt static library unavailable")
            compilation = subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-fbracket-depth=1024",
                    *includes,
                    str(cpp),
                    *compiled,
                    str(fmt),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                timeout=90,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stderr.decode())
            subprocess.run([str(binary), temp], check=True, timeout=10)
            gs = (Path(temp) / "gs.hlsl").read_text()
            self.assertIn("inputs[1u].attr7.x", gs)
            self.assertIn("NX_XFB_POSITION", gs)
            self.assertLess(
                gs.index("nx_vertex.nx_xfb_position = nx_vertex.position"),
                gs.index("nx_vertex.position.y = -nx_vertex.position.y"),
            )
            dxc = os.environ.get("DXC") or shutil.which("dxc")
            local = Path.home() / "Work/dxc/build/bin/dxc"
            if not dxc and local.is_file():
                dxc = str(local)
            if dxc:
                env = os.environ.copy()
                if local.is_file():
                    env["DYLD_LIBRARY_PATH"] = str(local.parent.parent / "lib")
                for stage in ("vs", "gs", "ps"):
                    subprocess.run(
                        [
                            dxc,
                            str(Path(temp) / f"{stage}.hlsl"),
                            "-T",
                            f"{stage}_6_0",
                            "-E",
                            "main",
                            "-Fo",
                            str(Path(temp) / f"{stage}.dxil"),
                        ],
                        check=True,
                        capture_output=True,
                        env=env,
                        timeout=30,
                    )

    def test_draw_texture_shaders_compile(self):
        dxc = os.environ.get("DXC") or shutil.which("dxc")
        local = Path.home() / "Work/dxc/build/bin/dxc"
        if not dxc and local.is_file():
            dxc = str(local)
        if not dxc:
            self.skipTest("DXC unavailable")
        env = os.environ.copy()
        if local.is_file():
            env["DYLD_LIBRARY_PATH"] = str(local.parent.parent / "lib")
        header = (ROOT / "src/shader_recompiler/backend/hlsl/hlsl_draw_texture.h").read_text()
        shaders = re.findall(r'R"hlsl\((.*?)\)hlsl"', header, re.S)
        self.assertEqual(len(shaders), 2)
        with tempfile.TemporaryDirectory() as temp:
            for stage, source in zip(("vs", "ps"), shaders):
                path = Path(temp) / f"{stage}.hlsl"
                path.write_text(source)
                subprocess.run(
                    [
                        dxc,
                        str(path),
                        "-E",
                        "main",
                        "-T",
                        f"{stage}_6_0",
                        "-Fo",
                        str(Path(temp) / f"{stage}.dxil"),
                    ],
                    env=env,
                    check=True,
                    capture_output=True,
                    timeout=30,
                )

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
