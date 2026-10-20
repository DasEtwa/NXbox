# SPDX-License-Identifier: GPL-3.0-or-later
"""Count production alias preparation and compare writes/copies with the opt-out path."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_texture_cache_fastpath import ROOT, SOURCE, function


class TextureAliasSyncTests(unittest.TestCase):
    def test_alias_preparation_and_write_invalidation(self):
        source = SOURCE.read_text()
        methods = "\n".join(
            function(source, name)
            for name in (
                "void TextureCache<P>::InvalidateAliasSync",
                "void TextureCache<P>::MarkModification(ImageBase&",
                "void TextureCache<P>::SynchronizeAliases",
                "void TextureCache<P>::PrepareImage(",
                "void TextureCache<P>::PrepareImageView",
            )
        )
        base = (ROOT / "src/video_core/texture_cache/texture_cache_base.h").read_text()
        switch = base[base.index("    const bool alias_sync_fastpath") :]
        switch = switch[: switch.index("    u64 alias_sync_generation")]
        harness = (ROOT / "tests/port/texture_alias_sync.cpp").read_text()
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(
                harness.replace("// METHODS", methods).replace("// ALIAS_SWITCH", switch)
            )
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
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for setting in ("1", "0"):
                result = subprocess.run(
                    [str(binary)],
                    capture_output=True,
                    text=True,
                    env={**os.environ, "NXBOX_TEXTURE_ALIAS_SYNC_FASTPATH": setting},
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_sampler_slot_content_invalidation_and_collisions(self):
        source = SOURCE.read_text()
        base = (ROOT / "src/video_core/texture_cache/texture_cache_base.h").read_text()
        storage = base[base.index("    struct ResolvedSampler {") :]
        storage = storage[: storage.index("    TextureCacheGPUMap* gpu_page_table")]
        switch = base[base.index("    const bool sampler_slot_fastpath") :]
        switch = switch[: switch.index("    struct CachedImageLookup")]
        harness = (ROOT / "tests/port/texture_sampler_slots.cpp").read_text()
        harness = harness.replace(
            "// METHODS", function(source, "SamplerId TextureCache<P>::GetSamplerId")
        )
        harness = harness.replace("// SAMPLER_STORAGE", storage).replace(
            "// SAMPLER_SWITCH", switch
        )
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(harness)
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
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for setting in ("1", "0"):
                result = subprocess.run(
                    [str(binary)],
                    capture_output=True,
                    text=True,
                    env={**os.environ, "NXBOX_TEXTURE_SAMPLER_SLOT_FASTPATH": setting},
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_topology_and_upload_invalidation(self):
        source = SOURCE.read_text()
        for name in ("InsertImage", "JoinImages", "DeleteImage", "InvalidateScale"):
            self.assertIn("++alias_sync_generation;", function(source, f"TextureCache<P>::{name}("))
        self.assertIn(
            "image.alias_sync_frame_tick =", function(source, "TextureCache<P>::RefreshContents(")
        )


if __name__ == "__main__":
    unittest.main()
