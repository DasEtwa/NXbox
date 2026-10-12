# SPDX-License-Identifier: GPL-3.0-or-later
"""Executable coverage of every Maxwell pixel format and conversion family."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/video_core/renderer_d3d12/d3d12_formats.h"
DRIVER = r"""
#include <cassert>
#include <array>
#include <cstring>
#include <random>
#include "video_core/renderer_d3d12/d3d12_formats.h"
using namespace D3D12;
int main() {
    for (const auto& format : FormatTable) {
        assert(format.Supported());
        assert(format.view_format && format.bytes_per_block);
        assert(format.planes == (format.conversion == FormatConversion::DepthStencil ? 2u : 1u));
    }
    assert(!LookupFormat("Invalid").Supported());
    for (const auto name : {"B5G6R5_UNORM", "A1B5G5R5_UNORM", "A5B5G5R1_UNORM", "A4B4G4R4_UNORM", "G4R4_UNORM"}) {
        for (unsigned bits = 0; bits < (1u << (8 * PackedBytes(name))); ++bits) {
            std::array<std::uint8_t, 4> guest{}, restored{}, host{};
            std::memcpy(guest.data(), &bits, PackedBytes(name));
            ConvertPackedRow(name, host.data(), guest.data(), 1, false);
            ConvertPackedRow(name, host.data(), restored.data(), 1, true);
            assert(guest == restored);
        }
    }
    for (unsigned bits = 0; bits < 100000; ++bits) {
        const std::uint32_t value = bits * 42949u;
        std::array<std::uint8_t, 4> guest{}, restored{};
        std::array<std::uint8_t, 8> host{};
        std::memcpy(guest.data(), &value, 4);
        ConvertPackedRow("A2R10G10B10_UNORM", host.data(), guest.data(), 1, false);
        ConvertPackedRow("A2R10G10B10_UNORM", host.data(), restored.data(), 1, true);
        assert(guest == restored);
    }
    // Canonical host channels must match the guest's bit significance.
    std::array<std::uint8_t, 4> guest{31, 0, 0, 0}, host{};
    ConvertPackedRow("B5G6R5_UNORM", host.data(), guest.data(), 1, false);
    assert(host[0] == 255 && host[1] == 0 && host[2] == 0 && host[3] == 255);
    guest = {1, 0, 0, 0};
    ConvertPackedRow("A5B5G5R1_UNORM", host.data(), guest.data(), 1, false);
    assert(host[0] == 255 && host[3] == 0);
    for (const auto name : {"D24_UNORM_S8_UINT", "S8_UINT_D24_UNORM", "D32_FLOAT_S8_UINT", "S8_UINT", "X8_D24_UNORM"}) {
        std::array<std::uint8_t, 8> packed{0x12,0x34,0x56,0x78,0x90,0,0,0}, restored = packed;
        std::array<std::uint8_t, 4> depth{}, stencil{};
        ConvertDepthRow(name, depth.data(), packed.data(), 1, 0, false);
        ConvertDepthRow(name, stencil.data(), packed.data(), 1, 1, false);
        if (std::string_view(name) == "S8_UINT_D24_UNORM") assert(depth[0] == 0x34 && stencil[0] == 0x12);
        if (std::string_view(name) == "D24_UNORM_S8_UINT") assert(depth[0] == 0x12 && stencil[0] == 0x78);
        if (std::string_view(name) == "D32_FLOAT_S8_UINT") assert(stencil[0] == 0x90);
        // Preserve padding/X bits, which D3D12 does not expose as depth/stencil.
        ConvertDepthRow(name, depth.data(), restored.data(), 1, 0, true);
        ConvertDepthRow(name, stencil.data(), restored.data(), 1, 1, true);
        assert(restored == packed);
    }
    std::array<std::uint8_t, 16> compressed{};
    std::array<std::uint8_t, 64> decoded{};
    DecodeEtcBlock(LookupFormat("ETC2_RGB_UNORM"), compressed.data(), decoded.data());
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        assert(decoded[pixel*4] == 2 && decoded[pixel*4+1] == 2 && decoded[pixel*4+2] == 2);
        assert(decoded[pixel*4+3] == 255);
    }
    DecodeEtcBlock(LookupFormat("ETC2_RGB_PTA_UNORM"), compressed.data(), decoded.data());
    for (unsigned pixel = 0; pixel < 16; ++pixel) assert(decoded[pixel*4+3] == 255);
    DecodeEtcBlock(LookupFormat("ETC2_RGBA_UNORM"), compressed.data(), decoded.data());
    for (unsigned pixel = 0; pixel < 16; ++pixel) assert(decoded[pixel*4+3] == 0);
    // Signed EAC decodes a signed base codeword before applying the modifier,
    // rather than reinterpreting unsigned normalized output as signed data.
    compressed[0] = 0x80;
    DecodeEtcBlock(LookupFormat("EAC_R11_SNORM"), compressed.data(), decoded.data());
    std::int16_t signed_sample{};
    std::memcpy(&signed_sample, decoded.data(), 2);
    assert(signed_sample == (-1019 * 32767 / 1023));
    compressed.fill(0);
    DecodeEtcBlock(LookupFormat("EAC_R11_UNORM"), compressed.data(), decoded.data());
    std::uint16_t sample{};
    std::memcpy(&sample, decoded.data(), 2);
    assert(sample == 65535 / 2047);
    // Exercise T/H/planar modes, all selectors, odd/aligned input addresses and
    // both EAC channels under ASan/UBSan without any copyrighted game data.
    std::mt19937 random(0xD312);
    for (unsigned i = 0; i < 10000; ++i) {
        std::array<std::uint8_t, 17> input{};
        for (auto& value : input) value = static_cast<std::uint8_t>(random());
        for (const auto& format : FormatTable) {
            if (format.conversion == FormatConversion::Etc2 || format.conversion == FormatConversion::Eac)
                DecodeEtcBlock(format, input.data() + 1, decoded.data());
        }
    }
}
"""


class D3D12FormatCoverageTests(unittest.TestCase):
    def test_every_pixel_format_has_host_storage(self):
        surface = (ROOT / "src/video_core/surface.h").read_text()
        names = re.findall(r"PIXEL_FORMAT_ELEM\((\w+),\s*\d+,", surface)
        rows = re.findall(r'\{"(\w+)",\s*(\d+),\s*(\d+)', HEADER.read_text())
        self.assertEqual(len(names), len(rows))
        self.assertEqual(set(names), {row[0] for row in rows})
        self.assertEqual(
            [], [name for name, resource, view in rows if int(resource) == 0 or int(view) == 0]
        )

    def test_conversions_and_decoder_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t1-formats-") as temp:
            source = Path(temp) / "formats.cpp"
            binary = Path(temp) / "formats"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_plane_selection_and_upload_dispatch(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        for token in [
            "TransferConverted(input, copies, download)",
            "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
            "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
            "view_plane",
            "ConvertDepthRow",
            "DecodeEtcBlock",
            "ConvertPackedRow",
            "plane * static_cast<UINT>",
        ]:
            self.assertIn(token, source)
