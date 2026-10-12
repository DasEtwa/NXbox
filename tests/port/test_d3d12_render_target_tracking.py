# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the common Maxwell dirty tables consumed by native render targets."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAXWELL = r"""
#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
namespace Tegra::Engines {
struct Maxwell3D {
    struct Regs {
        static constexpr std::size_t NumVertexArrays = 32;
        static constexpr std::size_t NumRenderTargets = 8;
        static constexpr std::size_t MaxShaderProgram = 6;
        u32 vertex_streams[32][3], vertex_stream_limits[32];
        u32 index_buffer[4], tex_header[4], tex_sampler[4];
        u32 rt[8][16], surface_clip[4], rt_control;
        u32 zeta_enable;
        struct { u32 width, height; } zeta_size;
        u32 zeta[8], pipelines[6][4];
    };
    struct DirtyState {
        using Table = std::array<u8, 4096>;
        using Tables = std::array<Table, 2>;
        Tables tables{};
        std::bitset<256> flags;
    };
};
}
#define MAXWELL3D_REG_INDEX(field) (offsetof(Tegra::Engines::Maxwell3D::Regs, field) / sizeof(u32))
"""
DRIVER = r"""
#include <cassert>
#include "video_core/dirty_flags.h"
int main() {
    using namespace VideoCommon::Dirty;
    Tegra::Engines::Maxwell3D::DirtyState dirty;
    SetupDirtyFlags(dirty.tables);
    dirty.flags.set();
    assert(dirty.flags[RenderTargets] && dirty.flags[ZetaBuffer]);
    const auto write = [&](std::size_t method) {
        dirty.flags.reset();
        for (const auto& table : dirty.tables) dirty.flags[table[method]] = true;
    };
    for (std::size_t target = 0; target < 8; ++target) {
        for (std::size_t word = 0; word < 16; ++word) {
            write(MAXWELL3D_REG_INDEX(rt) + target * 16 + word);
            assert(dirty.flags[ColorBuffer0 + target] && dirty.flags[RenderTargets]);
        }
    }
    write(MAXWELL3D_REG_INDEX(rt_control));
    assert(dirty.flags[RenderTargetControl] && dirty.flags[RenderTargets]);
    for (auto method : {MAXWELL3D_REG_INDEX(zeta_enable),
                        MAXWELL3D_REG_INDEX(zeta_size.width),
                        MAXWELL3D_REG_INDEX(zeta_size.height),
                        MAXWELL3D_REG_INDEX(zeta)}) {
        write(method);
        assert(dirty.flags[ZetaBuffer] && dirty.flags[RenderTargets]);
    }
    write(MAXWELL3D_REG_INDEX(surface_clip));
    assert(dirty.flags[RenderTargets]);
}
"""


class D3D12RenderTargetTrackingTests(unittest.TestCase):
    def test_maxwell_attachment_writes_invalidate_cached_framebuffer(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-targets-") as temp:
            temp = Path(temp)
            engine = temp / "video_core/engines/maxwell_3d.h"
            engine.parent.mkdir(parents=True)
            engine.write_text(MAXWELL)
            types = temp / "common/common_types.h"
            types.parent.mkdir()
            types.write_text('#include "video_core/engines/maxwell_3d.h"\n')
            source = temp / "targets.cpp"
            source.write_text(DRIVER)
            binary = temp / "targets"
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "src/video_core/dirty_flags.cpp"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_native_channel_installs_tracking_before_cache_creation(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        channel = source.split("void RasterizerD3D12::InitializeChannel", 1)[1]
        self.assertLess(channel.index("SetupDirtyFlags"), channel.index("CreateChannel"))
        self.assertIn("dirty.flags.set()", channel)
        draw = source.split("void RasterizerD3D12::PrepareDraw", 1)[1]
        self.assertLess(
            draw.index("UpdateRenderTargets(false)"), draw.index("CurrentGraphicsPipeline")
        )
