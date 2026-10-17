# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute native format, footprint and staging row packing on the host."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <algorithm>
#include <array>
#include <vector>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_footprint.h"
using namespace D3D12;
template <typename Exception, typename F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const Exception&) { rejected = true; }
    assert(rejected);
}
int main() {
    // DXGI SDK enum values: typeless storage and typed views share a cast family.
    constexpr auto rgba = LookupFormat("A8B8G8R8_UNORM");
    static_assert(rgba.resource_format == 27 && rgba.view_format == 28);
    static_assert(LookupFormat("A8B8G8R8_SRGB").view_format == 29);
    static_assert(LookupFormat("A8B8G8R8_UINT").resource_format == 27);
    static_assert(LookupFormat("B8G8R8A8_UNORM").resource_format == 90);
    static_assert(LookupFormat("B8G8R8A8_SRGB").view_format == 91);
    static_assert(LookupFormat("BC1_RGBA_UNORM").resource_format == 70);
    static_assert(LookupFormat("BC1_RGBA_SRGB").view_format == 72);
    static_assert(LookupFormat("D32_FLOAT").resource_format == 39);
    static_assert(LookupFormat("D16_UNORM").view_format == 56);
    static_assert(LookupFormat("D24_UNORM_S8_UINT").planes == 2);
    static_assert(LookupFormat("R5G6B5_UNORM").view_format == 85);
    static_assert(LookupFormat("B5G6R5_UNORM").Supported());
    static_assert(LookupFormat("S8_UINT_D24_UNORM").Supported());
    static_assert(LookupFormat("ETC2_RGBA_UNORM").Supported());
    static_assert(!LookupFormat("Invalid").Supported());
    for (const auto& format : FormatTable) {
        assert(!format.name.empty());
        if (format.converted_astc) {
            assert(format.resource_format == 27 && format.bytes_per_block == 4);
            assert(format.block_width == 1 && format.block_height == 1);
        }
        for (const auto& other : FormatTable)
            assert(&format == &other || format.name != other.name);
    }
    // A non-aligned row and a non-aligned base are independently padded.
    constexpr auto small = MakeFootprint(rgba, 17, 3, 1, 1);
    static_assert(small.offset == 512 && small.row_bytes == 68);
    static_assert(small.row_pitch == 256 && small.rows == 3 && small.End() == 1280);
    const auto full = MakeFootprint(rgba, 1920, 1080);
    assert(full.row_pitch == 7680 && full.size == 8294400);
    assert(MakeFootprint(rgba, 65, 1).row_pitch == 512);
    assert(MakeFootprint(rgba, 64, 1).row_pitch == 256);
    // BC edge mips retain at least one block, including odd widths/heights.
    constexpr auto bc = LookupFormat("BC1_RGBA_UNORM");
    const auto odd = MakeFootprint(bc, 7, 5, 2, 513);
    assert(odd.offset == 1024 && odd.row_bytes == 16 && odd.rows == 2);
    assert(odd.row_pitch == 256 && odd.slice_pitch == 512 && odd.size == 1024);
    assert(MakeFootprint(bc, 1, 1).row_bytes == 8);
    assert(MakeFootprint(LookupFormat("BC7_UNORM"), 1, 1).row_bytes == 16);
    // Chain mip/layer allocations with 512-byte placement alignment.
    std::uint64_t end = 0;
    for (unsigned layer = 0; layer < 3; ++layer) {
        for (unsigned level = 0; level < 5; ++level) {
            const auto mip = MakeFootprint(rgba, 32u >> level, 32u >> level, 1, end);
            assert(mip.offset % 512 == 0 && mip.offset >= end && mip.row_pitch % 256 == 0);
            end = mip.End();
        }
    }
    rejects<std::invalid_argument>([&]{ MakeFootprint(rgba, 0, 1); });
    rejects<std::invalid_argument>([&]{ MakeFootprint(LookupFormat("Invalid"), 1, 1); });
    rejects<std::overflow_error>([&]{ MakeFootprint(rgba, UINT32_MAX, 1); });
    rejects<std::overflow_error>([&]{ MakeFootprint(rgba, 1, 1, 1, UINT64_MAX); });
    rejects<std::overflow_error>([&]{ CheckedMultiply(UINT64_MAX, 2); });
    rejects<std::invalid_argument>([&]{ Align(10, 3); });
    // Round-trip two slices with padded guest rows and a staging offset.
    constexpr unsigned row_bytes = 68, rows = 3, depth = 2, guest_pitch = 80;
    std::vector<std::uint8_t> guest(guest_pitch * rows * depth, 0xa5);
    for (unsigned z = 0; z < depth; ++z)
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < row_bytes; ++x)
                guest[z * guest_pitch * rows + y * guest_pitch + x] = (x + y * 7 + z * 53) & 255;
    std::vector<std::uint8_t> staging(512 + 256 * rows * depth, 0xcc);
    const auto placed = std::span(staging).subspan(512);
    RepackRows(placed, guest, row_bytes, rows, depth, 256, 256 * rows, guest_pitch, guest_pitch * rows);
    std::vector<std::uint8_t> output(guest.size(), 0xa5);
    RepackRows(output, placed, row_bytes, rows, depth, guest_pitch, guest_pitch * rows, 256, 256 * rows);
    assert(output == guest);
    // DMA uses one padded DEFAULT buffer for GPU storage and CPU readback.
    // Its row plan must produce exactly the same bytes as CPU repacking.
    std::vector<std::uint8_t> gpu_output(32 + guest.size(), 0xa5);
    int copies = 0;
    ForEachRowCopy(gpu_output.size(), staging.size(), 32, 512, row_bytes, rows, depth,
                   guest_pitch, guest_pitch * rows, 256, 256 * rows,
                   [&](std::uint64_t dst, std::uint64_t src, std::uint64_t size) {
                       ++copies;
                       std::memcpy(gpu_output.data() + dst, staging.data() + src, size);
                   });
    assert(copies == rows * depth);
    assert(std::equal(guest.begin(), guest.end(), gpu_output.begin() + 32));
    copies = 0;
    rejects<std::out_of_range>([&] {
        ForEachRowCopy(10, staging.size(), 32, 512, row_bytes, rows, depth,
                       guest_pitch, guest_pitch * rows, 256, 256 * rows,
                       [&](auto, auto, auto) { ++copies; });
    });
    assert(copies == 0);
    assert(staging[0] == 0xcc && placed[row_bytes] == 0xcc);
    rejects<std::out_of_range>([&]{ RepackRows(std::span(output).first(10), placed, row_bytes, rows, depth, guest_pitch, guest_pitch * rows, 256, 256 * rows); });
    rejects<std::invalid_argument>([&]{ RepackRows(output, placed, row_bytes, rows, depth, 1, guest_pitch * rows, 256, 256 * rows); });
}
"""


class D3D12ResourceTests(unittest.TestCase):
    def test_format_coverage(self):
        surface = (ROOT / "src/video_core/surface.h").read_text()
        names = re.findall(r"PIXEL_FORMAT_ELEM\((\w+),\s*\d+,", surface)
        table = (ROOT / "src/video_core/renderer_d3d12/d3d12_formats.h").read_text()
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(set(names), set(re.findall(r'\{"(\w+)"', table)))

    def test_formats_footprints_and_round_trip(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-resources-") as temp:
            source = Path(temp) / "resources.cpp"
            binary = Path(temp) / "resources"
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
            subprocess.run([str(binary)], check=True, timeout=10)
