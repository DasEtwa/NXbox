# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable texture transfer policies and Windows recording contracts."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"
DRIVER = r"""
#include <cassert>
#include <cstdint>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_texture_transfer.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"
using namespace D3D12;
int main() {
    assert(IsExactBlit(0, 0, 1920, 1080, 0, 0, 1920, 1080));
    assert(IsExactBlit(10, 20, 50, 60, 3, 4, 43, 44));
    assert(!IsExactBlit(-1, 0, 99, 100, 0, 0, 100, 100));
    assert(!IsExactBlit(100, 0, 0, 100, 0, 0, 100, 100));
    assert(!IsExactBlit(0, 0, 0, 100, 0, 0, 0, 100));
    assert(!IsExactBlit(0, 0, 100, 100, 0, 0, 200, 100));
    const auto rgba = LookupFormat("A8B8G8R8_UNORM");
    const auto full = MakeFootprint(rgba, 1920, 1080);
    assert(full.size == 8294400);
    assert(CanDirectTextureUpload(512, 7680, 8294400, full.row_pitch, full.slice_pitch));
    assert(!CanDirectTextureUpload(516, 7680, 8294400, full.row_pitch, full.slice_pitch));
    assert(!CanDirectTextureUpload(512, 7680, 8294404, full.row_pitch, full.slice_pitch));
    // Compare actual bytes to the previous repack path, including mips, padded
    // widths, slices and suballocations. Only identical layouts bypass repacking.
    for (std::uint32_t width : {1u, 63u, 64u, 65u, 960u, 1920u}) {
        const auto footprint = MakeFootprint(rgba, width, 3, 2);
        const std::uint64_t stride = width * 4, slice = stride * 3;
        std::vector<std::uint8_t> tight(slice * 2), padded(footprint.size);
        for (std::size_t i = 0; i < tight.size(); ++i) tight[i] = i % 251;
        RepackRows(padded, tight, stride, 3, 2, footprint.row_pitch,
                   footprint.slice_pitch, stride, slice);
        const bool direct = CanDirectTextureUpload(1024, stride, slice,
                                                   footprint.row_pitch, footprint.slice_pitch);
        assert(direct == (width % 64 == 0));
        if (direct) assert(tight == padded);
    }
    assert(CoversSingleSubresource(0, 0, 1920, 1080, 1920, 1080, 1, 1, 1));
    assert(!CoversSingleSubresource(1, 0, 1920, 1080, 1920, 1080, 1, 1, 1));
    assert(!CoversSingleSubresource(0, 0, 1919, 1080, 1920, 1080, 1, 1, 1));
    assert(!CoversSingleSubresource(0, 0, 1920, 1080, 1920, 1080, 2, 1, 1));
    assert(!CoversSingleSubresource(0, 0, 1920, 1080, 1920, 1080, 1, 2, 1));
    assert(!CoversSingleSubresource(0, 0, 1920, 1080, 1920, 1080, 1, 1, 2));
    RepeatedTextureUploads uploads;
    uploads.Add({1, 10000000, 0, 1, 2500, 1000, "initial", false});
    uploads.Add({2, 8294400, 0, 2, 1920, 1080, "cpu_write_notification", true});
    uploads.Add({2, 8294400, 0, 2, 1920, 1080, "fermi_software_write", false});
    auto winner = uploads.LargestRepeated();
    assert(winner.address == 2 && winner.count == 2);
    assert(winner.reason == "fermi_software_write" && !winner.gpu_modified);
    for (unsigned i = 3; i < 200; ++i)
        uploads.Add({i, 4, 0, 1, 1, 1, "initial", false});
    assert(uploads.LargestRepeated().address == 2);
    uploads.Reset();
    assert(uploads.LargestRepeated().count == 0);
    uploads.Add({2, 8294400, 0, 2, 1920, 1080, "initial", false});
    uploads.Add({2, 8294400, 0, 3, 1920, 1080, "initial", false});
    assert(uploads.LargestRepeated().count == 0); // Different formats are not repetitions.
}
"""


def test_transfer_policies_and_repeated_upload_selection():
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
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
        subprocess.run([str(binary)], check=True)


def test_preflight_precedes_cache_modification_and_preserves_fallback():
    cache = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
    blit = cache.split("bool TextureCache<P>::BlitImage(", 1)[1].split("\ntemplate <class P>", 1)[0]
    assert blit.index("runtime.CanBlitImages") < blit.index("PrepareImage(src_id")
    assert "src_id != dst_id" in blit
    assert "slot_images[dst_id].aliased_images.empty()" in blit
    assert "PrepareImage(dst_id, true, discard_dst)" in blit
    native = (NATIVE / "d3d12_texture_cache.cpp").read_text()
    preflight = native.split("bool TextureCacheRuntime::CanBlitImages(", 1)[1].split(
        "void TextureCacheRuntime::BlitImage(", 1
    )[0]
    for contract in [
        "TryFindBase",
        "CopyCompatible",
        "num_samples != 1",
        "IsExactBlit",
        "FormatConversion::None",
        "converted_astc",
        "fits(source",
        "fits(target",
        "DrawCompatible",
    ]:
        assert contract in preflight
    # Depth blits through color Fermi surfaces are drawn when both images are depth.
    assert "dst.HostFormat().planes != src.HostFormat().planes" in preflight
    assert "format.depth && format.conversion == FormatConversion::DepthStencil" in preflight
    assert preflight.index("if (depth)\n        return true;") < preflight.index("DrawCompatible")
    runtime_blit = native.split("void TextureCacheRuntime::BlitImage(Framebuffer*", 1)[1]
    assert "BlitHelpers().BlitDepthStencil(d, s, dst_region, src_region)" in runtime_blit
    rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
    blit = rasterizer.split("bool RasterizerD3D12::AccelerateSurfaceCopy(", 1)[1].split(
        "void RasterizerD3D12::AccelerateInlineToMemory", 1
    )[0]
    assert "texture_cache.BlitImage(dst, src, gpu_copy)" in blit
    # Scaled and format-changing color blits are drawn; depth, ROP and converted formats
    # still take the software path.
    assert "plain(format) && plain(src_format)" in blit
    assert "Operation::SrcCopy" in blit
    for counter in [
        "FermiBlitsAccelerated",
        "FermiBlitsSoftware",
        "FermiBlitAcceleratedBytes",
        "FermiBlitSoftwareBytes",
    ]:
        assert counter in blit


def test_direct_upload_retains_input_and_limits_diagnostic_rate():
    source = (NATIVE / "d3d12_texture_cache.cpp").read_text()
    assert "StagingBufferRef{map.buffer" in source
    assert "context.Retain(aligned.buffer)" in source
    assert "padded_size <= map.mapped_span.size() - base" in source
    assert "TextureDirectUploadBytes" in source and "TextureRepackBytes" in source
    assert "std::chrono::seconds{5}" in source
    assert "D3D12_TEXTURE_UPLOAD_REPEAT gpu_address={:#x} bytes={} format={}" in source
    assert "Fermi2D::IsSoftwareBlitActive()" in source
    assert "TextureRefreshBytes" in source
