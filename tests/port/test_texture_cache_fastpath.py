# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production lookup/preparation paths with a counted host cache backend."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/video_core/texture_cache/texture_cache.h"


def function(source, name):
    start = source.index("template <class P>\n", source.index(name) - 100)
    brace = source.index("{", source.index(name))
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class TextureCacheFastpath(unittest.TestCase):
    def test_counted_production_paths(self):
        source = SOURCE.read_text()
        methods = "\n".join(
            function(source, name)
            for name in (
                "ImageViewId TextureCache<P>::VisitImageView",
                "void TextureCache<P>::PrepareImageView",
            )
        )
        harness = (ROOT / "tests/port/texture_cache_fastpath.cpp").read_text()
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(harness.replace("// PRODUCTION_METHODS", methods))
            compile_result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(binary)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

            forced = subprocess.run(
                [str(binary), "force"],
                check=False,
                capture_output=True,
                text=True,
                env={**os.environ, "NXBOX_FORCE_REFRESH_DIMS": "64x64"},
            )
            self.assertEqual(forced.returncode, 0, forced.stderr)

    def test_exact_image_lookup_cost_and_selection(self):
        source = SOURCE.read_text()
        method = function(source, "ImageId TextureCache<P>::FindImage(")
        harness = (ROOT / "tests/port/texture_cache_lookup.cpp").read_text()
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(harness.replace("// PRODUCTION_METHOD", method))
            compiled = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_exact_query_key_compares_all_image_fields(self):
        header = (ROOT / "src/video_core/texture_cache/image_info.h").read_text()
        info = header[header.index("struct ImageInfo {") : header.index("\n};") + 3]
        import re

        info = re.sub(r"    explicit ImageInfo\([^;]*;\n", "", info)
        utility = (ROOT / "src/video_core/texture_cache/util.h").read_text()
        helper = utility[utility.index("[[nodiscard]] inline bool SameImageLookupInfo") :]
        helper = helper[: helper.index("\n}\n") + 3]
        prelude = """
#include <cassert>
#include <compare>
using u32 = unsigned;
enum class PixelFormat { Invalid, Color };
enum class ImageType { e1D, Linear };
struct Extent3D { u32 width, height, depth; auto operator<=>(const Extent3D&) const = default; };
struct SubresourceExtent { int levels = 1, layers = 1; auto operator<=>(const SubresourceExtent&) const = default; };
"""
        checks = """
int main() {
    const ImageInfo original;
    const auto differs = [&](auto mutate) {
        ImageInfo changed = original;
        mutate(changed);
        assert(!SameImageLookupInfo(original, changed));
    };
    assert(SameImageLookupInfo(original, original));
"""
        for expression in (
            "type = ImageType::Linear",
            "format = PixelFormat::Color",
            "resources.levels++",
            "resources.layers++",
            "size.width++",
            "size.height++",
            "size.depth++",
            "block.width++",
            "block.height++",
            "block.depth++",
            "layer_stride++",
            "maybe_unaligned_layer_stride++",
            "num_samples++",
            "tile_width_spacing++",
            "rescaleable = true",
            "downscaleable = true",
            "forced_flushed = true",
            "dma_downloaded = true",
            "is_sparse = true",
        ):
            checks += f"    differs([](ImageInfo& value) {{ value.{expression}; }});\n"
        checks += """
    ImageInfo linear;
    linear.type = ImageType::Linear;
    linear.pitch = 128;
    ImageInfo other = linear;
    assert(SameImageLookupInfo(linear, other));
    other.pitch++;
    assert(!SameImageLookupInfo(linear, other));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "key.cpp"
            binary = Path(directory) / "key"
            cpp.write_text(prelude + info + helper + checks)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)

    def test_lookup_invalidation_and_opt_in(self):
        source = SOURCE.read_text()
        for name in ("RegisterImage", "UnregisterImage", "UnmapGPUMemory"):
            body = function(source, f"TextureCache<P>::{name}(")
            self.assertIn("cached_image_lookup = {};", body)
        find = function(source, "TextureCache<P>::FindImage(")
        self.assertLess(find.index("cached_image_lookup"), find.index("ForEachImageInRegion"))
        self.assertIn("exact_match", find)
        self.assertIn("SameImageLookupInfo", find)
        runtime = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.h").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_TEXTURE_CACHE_FASTPATH")', runtime)


if __name__ == "__main__":
    unittest.main()
