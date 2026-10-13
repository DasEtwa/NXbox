# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate raw pass decoding, occurrence matching, and shared capture wiring."""

import importlib.util
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("pass_diff", ROOT / "tools/nxbox/pass_diff.py")
pass_diff = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pass_diff)


def entry(seq, slot=0, address=4096, format_name="A8B8G8R8_UNORM"):
    return {
        "seq": seq,
        "slot": slot,
        "gpu_addr": address,
        "width": 1,
        "height": 1,
        "format": format_name,
        "bytes": 4,
        "file": f"pass_{seq:04}_c{slot}.raw",
    }


class PassDumpTests(unittest.TestCase):
    def test_packed_float_known_values(self):
        packed = (15 << 6) | ((16 << 6) << 11) | ((14 << 5) << 22)
        self.assertEqual(
            pass_diff.decode_raw(struct.pack("<I", packed), "B10G11R11_FLOAT", 1, 1),
            [(1.0, 2.0, 0.5)],
        )
        packed = 1 | (1 << 11) | (1 << 22)
        self.assertEqual(
            pass_diff.decode_raw(struct.pack("<I", packed), "B10G11R11_FLOAT", 1, 1),
            [(2**-20, 2**-20, 2**-19)],
        )
        self.assertEqual(pass_diff.unsigned_float(31 << 6, 6), math.inf)
        self.assertTrue(math.isnan(pass_diff.unsigned_float((31 << 5) | 1, 5)))

    def test_half_float_known_values(self):
        decoded = pass_diff.decode_raw(bytes.fromhex("003c00c000000100ff7b"), "R16_FLOAT", 5, 1)
        self.assertEqual(decoded, [(1, 0, 0), (-2, 0, 0), (0, 0, 0), (2**-24, 0, 0), (65504, 0, 0)])

    def test_rgba8_round_trip_and_bgra(self):
        raw = bytes([12, 34, 56, 78, 255, 0, 128, 255])
        decoded = pass_diff.decode_raw(raw, "A8B8G8R8_UNORM", 2, 1)
        self.assertEqual(
            bytes(round(v * 255) for pixel in decoded for v in pixel),
            bytes([12, 34, 56, 255, 0, 128]),
        )
        self.assertEqual(
            pass_diff.decode_raw(raw[:4], "B8G8R8A8_UNORM", 1, 1), [(56 / 255, 34 / 255, 12 / 255)]
        )
        for name in ("A8B8G8R8_SRGB", "B8G8R8A8_SRGB"):
            decoded = pass_diff.decode_raw(bytes([128, 128, 128, 255]), name, 1, 1)
            self.assertAlmostEqual(decoded[0][0], 0.21586050011389926)

    def test_remaining_formats_and_invalid_input(self):
        cases = [
            ("A2B10G10R10_UNORM", struct.pack("<I", 1023 | (512 << 10)), (1, 512 / 1023, 0)),
            ("R16G16B16A16_FLOAT", struct.pack("<4e", 1, 2, 3, 4), (1, 2, 3)),
            ("R32G32B32A32_FLOAT", struct.pack("<4f", 1, 2, 3, 4), (1, 2, 3)),
            ("R16G16_FLOAT", struct.pack("<2e", 1, 2), (1, 2, 0)),
            ("R8_UNORM", b"\xff", (1, 0, 0)),
            ("R32_FLOAT", struct.pack("<f", 0.5), (0.5, 0, 0)),
        ]
        for name, raw, expected in cases:
            with self.subTest(format=name):
                self.assertEqual(pass_diff.decode_raw(raw, name, 1, 1), [expected])
        with self.assertRaisesRegex(ValueError, "unsupported format"):
            pass_diff.decode_raw(b"", "R8_UINT", 1, 1)
        with self.assertRaisesRegex(ValueError, "byte count"):
            pass_diff.decode_raw(b"\0", "R16_FLOAT", 1, 1)

    def test_pairing_kth_occurrence_and_key_fields(self):
        a = [entry(4), entry(1), entry(5, slot=1), entry(6, address=99)]
        b = [entry(20), entry(10), entry(30, slot=1), entry(40, address=100)]
        pairs, unmatched_a, unmatched_b = pass_diff.pair_passes(a, b)
        self.assertEqual([(x["seq"], y["seq"]) for x, y in pairs], [(1, 10), (4, 20), (5, 30)])
        self.assertEqual(unmatched_a, [a[3]])
        self.assertEqual(unmatched_b, [b[3]])
        for field, value in (("width", 2), ("height", 2), ("format", "R8_UNORM"), ("slot", 3)):
            other = entry(1)
            other[field] = value
            self.assertEqual(pass_diff.pair_passes([entry(1)], [other])[0], [])
        other = entry(1)
        other["gpu_addr"] = "0x1000"
        self.assertEqual(len(pass_diff.pair_passes([entry(1)], [other])[0]), 1)

    def test_metrics(self):
        psnr, maximum = pass_diff.compare([(2, -1, 0.5)], [(3, 0, 0.5)])
        self.assertEqual((psnr, maximum), (math.inf, 1))
        psnr, maximum = pass_diff.compare([(0, 0, 0)], [(1, 0, 0)])
        self.assertAlmostEqual(psnr, 10 * math.log10(3))
        self.assertEqual(maximum, 1)
        with self.assertRaisesRegex(ValueError, "NaN"):
            pass_diff.compare([(math.nan, 0, 0)], [(0, 0, 0)])

    def test_first_divergent_and_preview(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            a, b, out = root / "a", root / "b", root / "out"
            for directory, sequences in ((a, (1, 3, 5)), (b, (2, 4, 6))):
                directory.mkdir()
                entries = [entry(seq) for seq in sequences]
                (directory / "manifest.jsonl").write_text("\n".join(json.dumps(e) for e in entries))
                for index, e in enumerate(entries):
                    raw = bytes([255 if directory == b and index > 0 else 0, 0, 0, 255])
                    (directory / e["file"]).write_bytes(raw)
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/nxbox/pass_diff.py"),
                    str(a),
                    str(b),
                    "--out",
                    str(out),
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("FIRST DIVERGENT: seq=3 file=pass_0003_c0.raw", result.stdout)
            self.assertLess(result.stdout.index("seq=1 file="), result.stdout.index("seq=3 file="))
            self.assertEqual(
                (out / "pass_0003_c0.ppm").read_bytes(),
                b"P6\n3 1\n255\n" + bytes([0, 0, 0, 255, 0, 0, 255, 0, 0]),
            )
            self.assertFalse((out / "pass_0001_c0.ppm").exists())
            lines = []
            self.assertIsNone(pass_diff.run(a, a, root / "identical", report=lines.append))
            self.assertEqual(lines[-1], "NO DIVERGENCE")

    def test_skipped_unsupported_and_unmatched(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            skipped, unsupported = entry(1), entry(2, format_name="R8_UINT")
            skipped["skipped"] = "msaa"
            (directory / unsupported["file"]).write_bytes(b"\0")
            (directory / "manifest.jsonl").write_text(
                json.dumps(skipped) + "\n" + json.dumps(unsupported)
            )
            lines = []
            pass_diff.run(directory, directory, directory / "out", report=lines.append)
            self.assertIn("skipped A=msaa B=msaa", lines[0])
            self.assertIn("unsupported format", lines[1])
            self.assertFalse((directory / "out").exists())
            other = directory / "other"
            other.mkdir()
            (other / "manifest.jsonl").write_text(json.dumps(entry(3, address=7)))
            lines = []
            pass_diff.run(directory, other, directory / "out", report=lines.append)
            self.assertTrue(any("UNMATCHED A" in line for line in lines))
            self.assertTrue(any("UNMATCHED B" in line for line in lines))

    def test_shared_cache_hooks(self):
        source = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        update = source.split("void TextureCache<P>::UpdateRenderTargets(bool is_clear) {", 1)[
            1
        ].split("\ntemplate <class P>", 1)[0]
        tick = source.split("void TextureCache<P>::TickFrame() {", 1)[1].split(
            "\ntemplate <class P>", 1
        )[0]
        self.assertIn("UpdatePassDumpRenderTargets();", update)
        self.assertIn("DumpPassRenderTargets();", tick)
        self.assertLess(
            tick.index("DumpPassRenderTargets();"), tick.index("RunGarbageCollector();")
        )
        self.assertIn('std::getenv("NXBOX_PASS_DUMP_FRAME")', source)
        self.assertIn('std::getenv("NXBOX_PASS_DUMP_DIR")', source)


if __name__ == "__main__":
    unittest.main()
