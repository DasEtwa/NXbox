# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check feedback decisions and nonfatal draw validation without a Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <initializer_list>
#include <cstdint>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_feedback.h"
#include "video_core/renderer_d3d12/d3d12_buffer_view.h"
int main() {
    using namespace D3D12;
    // Color always snapshots; depth snapshots when either plane can write.
    for (bool depth : {false, true}) {
        for (bool depth_writes : {false, true}) {
            for (bool stencil_writes : {false, true}) {
                const bool readonly = depth && !depth_writes && !stencil_writes;
                assert((SelectFeedbackMode(depth, depth_writes, stencil_writes) ==
                        FeedbackMode::ReadOnlyDepth) == readonly);
            }
        }
    }
    // D3D12 states: RTV=4, UAV=8, DEPTH_WRITE=16, DEPTH_READ=32, SRVs=64|128.
    constexpr std::uint32_t writes = 4 | 8 | 16;
    for (auto read : {1u, 2u, 32u, 64u, 128u, 192u, 224u}) {
        assert(CompatibleDrawStates(read, 192, writes));
        for (auto write : {4u, 8u, 16u}) {
            assert(!CompatibleDrawStates(read, write, writes));
            assert(!CompatibleDrawStates(write, read, writes));
            assert(CompatibleDrawStates(write, write, writes));
        }
    }
    assert(!CompatibleDrawStates(4, 8, writes));
    assert(!CompatibleDrawStates(16, 8, writes));
    // A buffer read through an SRV and written through a UAV by one draw stays in UAV
    // (Dragon Ball Sparking! Zero skipped 33-84 draws a frame on this); nothing else merges.
    for (auto read : {64u, 128u, 192u}) {
        assert(MergesIntoBufferUav(read, 8));
        assert(MergesIntoBufferUav(8, read));
        assert(MergesIntoBufferUav(8 | read, read));
    }
    assert(!MergesIntoBufferUav(8, 8));
    assert(!MergesIntoBufferUav(1, 8));
    assert(!MergesIntoBufferUav(4, 64));
    assert(!MergesIntoBufferUav(16, 64));
    assert(!MergesIntoBufferUav(64, 128));
    assert(IsValidBufferView(4096, 256, 128, 128));
    assert(!IsValidBufferView(0, 256, 0, 128));
    assert(!IsValidBufferView(4096, 256, 0, 0));
    assert(!IsValidBufferView(4096, 256, 257, 1));
    assert(!IsValidBufferView(4096, 256, 128, 129));
    constexpr auto max64 = std::numeric_limits<std::uint64_t>::max();
    constexpr auto max32 = std::numeric_limits<std::uint32_t>::max();
    assert(!IsValidBufferView(max64 - 1, 256, 2, 1));
    assert(!IsValidBufferView(max64 - 1, 256, 0, 2));
    // UINT-sized geometry views cannot silently truncate a 4 GiB guest range.
    assert(IsValidBufferView(4096, max64, 0, max32));
    assert(!IsValidBufferView(4096, max64, 0, std::uint64_t{max32} + 1));
}
"""


class D3D12FeedbackTests(unittest.TestCase):
    def test_buffer_uav_merge_only_applies_to_buffers(self):
        for name in ("d3d12_rasterizer.cpp", "d3d12_dispatch.cpp"):
            source = (ROOT / "src/video_core/renderer_d3d12" / name).read_text()
            merge = source.index("MergesIntoBufferUav(")
            self.assertIn("D3D12_RESOURCE_DIMENSION_BUFFER", source[merge : merge + 400])

    def test_compute_reads_beside_writes_split_by_subresource(self):
        native = ROOT / "src/video_core/renderer_d3d12"
        rasterizer = (native / "d3d12_rasterizer.cpp").read_text()
        compute = rasterizer[rasterizer.index("void RasterizerD3D12::DispatchCompute()") :]
        compute = compute[: compute.index("dispatch.split_reads = split_reads;")]
        # Texture reads wait for the image writes, then split only when no subresource overlaps.
        self.assertLess(compute.index("image_reads.push_back"), compute.index("for (auto& read : image_reads)"))
        decision = compute[compute.index("for (auto& read : image_reads)") :]
        self.assertIn("written && !overlaps && !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)", decision)
        self.assertIn("use(read.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)", decision)
        # The split reads move after the whole-resource batch put the written texture in UAV.
        bind = (native / "d3d12_dispatch.cpp").read_text()
        batch = bind.index("context.TransitionBatch(transitions);")
        self.assertLess(batch, bind.index("bindings.split_reads", batch))
        self.assertIn("TransitionSubresources(resource, subresources,", bind[batch : batch + 300])

    def test_feedback_and_draw_validation(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-feedback-") as temp:
            source = Path(temp) / "feedback.cpp"
            binary = Path(temp) / "feedback"
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
