# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the shader/input layout and framebuffer PSO contracts without an SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cstring>
#include "shader_recompiler/backend/hlsl/hlsl_vertex_semantic.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
using namespace D3D12::Pso;
int main() {
    for (std::uint32_t i = 0; i < 32; ++i) {
        const auto semantic = Shader::Backend::HLSL::GenericSemantic(i);
        assert(std::strcmp(semantic.name, "TEXCOORD") == 0 && semantic.index == i);
    }
    GraphicsPsoState state{};
    state.attributes[0] = {1, 0, 12, 6, 1, 0, 0, 0};
    state.attributes[2] = {1, 3, 8, 6, 0, 0, 0, 0};
    ApplyVertexReads(state, 3);
    assert(state.attributes[0].slot == 0 && state.attributes[0].offset == 12);
    assert(state.attributes[1].enabled == 2 && state.attributes[1].slot == 1);
    assert(state.attributes[1].dxgi == 2 && state.attributes[1].offset == 0);
    assert(state.attributes[2].enabled == 0 && state.attributes[2].dxgi == 0);
    // 31 occupied slots still leave one legal zero-buffer slot for input 31.
    state = {};
    for (std::uint8_t i = 0; i < 31; ++i) state.attributes[i] = {1, i, 0, 6, 0, 0, 0, 0};
    ApplyVertexReads(state, 0xffffffffu);
    assert(state.attributes[31].slot == 31 && state.attributes[31].enabled == 2);
    state = {};
    state.depth_enable = state.depth_write = state.stencil_enable = 1;
    FramebufferFormats formats{};
    formats.colors[0] = 24; // R10G10B10A2_UNORM
    formats.colors[2] = 29; // R8G8B8A8_UNORM_SRGB; slot 1 is absent
    formats.depth = 55; // D16_UNORM
    ApplyFramebufferFormats(state, formats);
    assert(state.num_render_targets == 3 && state.rtv_formats[1] == 0);
    assert(state.rtv_formats[2] == 29 && state.dsv_format == 55);
    assert(state.depth_enable && state.depth_write && state.stencil_enable);
    const auto hash = HashBytes(&state, sizeof(state));
    auto other = state;
    formats.depth = 20; // D32_FLOAT_S8X24_UINT
    ApplyFramebufferFormats(other, formats);
    assert(hash != HashBytes(&other, sizeof(other)));
    formats.depth = 55; formats.colors[2] = 28; // linear view of the same image
    ApplyFramebufferFormats(other, formats);
    assert(hash != HashBytes(&other, sizeof(other)));
    ApplyFramebufferFormats(state, {});
    assert(state.num_render_targets == 0 && state.dsv_format == 0);
    assert(!state.depth_enable && !state.depth_write && !state.stencil_enable);
    for (auto format : state.rtv_formats) assert(format == 0);
    // Removing the final color view drops the trailing null output, retaining interior holes.
    formats = {}; formats.colors[4] = 28;
    ApplyFramebufferFormats(state, formats);
    assert(state.num_render_targets == 5 && state.rtv_formats[3] == 0);
}
"""


class D3D12DrawContractTests(unittest.TestCase):
    def test_semantics_vertex_reads_and_attachment_keys(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-draw-") as temp:
            source = Path(temp) / "draw.cpp"
            binary = Path(temp) / "draw"
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
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)
