# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the conversion LRU and verify Windows-only profiling/lifetime wiring."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


def test_index_cache_policy():
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "index-cache"
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
                str(ROOT / "tests/port/d3d12_index_cache_test.cpp"),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary)], check=True)


def test_index_cache_profile_and_lifetime_wiring():
    context = (NATIVE / "d3d12_context.cpp").read_text()
    rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
    converter = (NATIVE / "d3d12_index_converter.cpp").read_text()
    for suffix in ("uint8", "restart", "strip_or_fan", "provoking", "inline", "cache_hits"):
        assert f'"index_conversion_{suffix}"' in context
    assert "D3D12_FRAME_PROFILE thread=" in context
    assert "counters[i], counts[i]" in context
    assert "cpu && buffer_cache.IsRegionGpuModified(*cpu, input.size())" in rasterizer
    assert "index_converter.ConvertCached(key, input, cache_hit)" in rasterizer
    cached = converter.split("ConvertedIndexBuffer IndexConverter::ConvertCached(")[1].split(
        "ConvertedIndexBuffer IndexConverter::Generate("
    )[0]
    assert "Common::CityHash64" in cached
    hit, miss = cached.split("cache_hit = false;", 1)
    assert "context.Retain(found->resource)" in hit
    assert "return *found" in hit
    assert "IndexConversion::Convert" not in hit and "CreateBuffer" not in hit
    assert "context.CreateBuffer(layout.bytes, D3D12_HEAP_TYPE_UPLOAD)" in miss
    assert "resource->allocation_size" in miss
    assert "upload.Request" not in cached
    assert "need(converted_indices->resource, D3D12_RESOURCE_STATE_INDEX_BUFFER)" in rasterizer
    assert "Retain(use.first)" in context
