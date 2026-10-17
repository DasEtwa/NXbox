# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guest corpus parsing, failure isolation, and optional real compiler integration."""

import importlib.util
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/nxbox/shader_corpus"
spec = importlib.util.spec_from_file_location("shader_corpus", TOOL / "run.py")
corpus = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = corpus
spec.loader.exec_module(corpus)

# One Maxwell scheduler word and an unconditional EXIT PT (flow test T).
# Graphics entry offset is SPH size=80, which is itself a non-scheduler location.
EXIT = 0xE30000000007000F


def environment(stage=5, base=0, workgroup=(1, 1, 1), maps=None, code=None, sph=None):
    """Generate guest input, not prebuilt IR or copyrighted game data."""
    maps = maps or [[], [], [], []]
    header = sph if sph is not None else bytes(80)
    if code is None:
        code = struct.pack("<2Q", 0, EXIT) if stage == 5 else header + struct.pack("<2Q", EXIT, 0)
    data = corpus.HEADER.pack(
        len(code), *(len(items) for items in maps), 0, 2, base, base, base + len(code) - 8, 1, stage
    )
    data += code
    for items, layout in zip(maps, ("<II", "<II", "<QI", "<QI")):
        for item in items:
            data += struct.pack(layout, *item)
    data += struct.pack("<4I", *workgroup, 0) if stage == 5 else header
    if stage == 3:
        data += bytes(32)
    return data


def cache(path, version, groups):
    data = corpus.MAGIC + struct.pack("<I", version)
    for group in groups:
        data += struct.pack("<I", len(group)) + b"".join(group)
        stage = corpus.HEADER.unpack_from(group[0])[-1]
        data += bytes(24 if stage == 5 else corpus.VERSIONS[version][1])
    path.write_bytes(data)
    return path


def dump(path, stages=("vb", "fs")):
    path.mkdir(parents=True, exist_ok=True)
    lines = ["pipeline=0123456789abcdef", "stages=va,vb,tcs,tes,gs,fs"]
    for stage in stages:
        binary = bytes(80) + struct.pack("<2Q", EXIT, 0)
        (path / (stage + ".maxwell.bin")).write_bytes(binary)
        lines.append(f"{stage} hash=1111111111111111 address=0 start=0 bytes={len(binary)}")
    manifest = path / "pipeline.txt"
    manifest.write_text("\n".join(lines) + "\n")
    return manifest


class ShaderCorpusLoaderTests(unittest.TestCase):
    def test_both_native_cache_versions_all_stages_and_maps(self):
        maps = [[(4, 2)], [(4, 3)], [((2 << 32) | 16, 123)], [((1 << 32) | 8, 2)]]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in corpus.VERSIONS:
                groups = [
                    [environment(5, maps=maps)],
                    [environment(stage) for stage in (6, 0, 1, 2, 3, 4)],
                ]
                path = cache(root / f"v{version}.bin", version, groups)
                cases = list(corpus.cache_cases(path))
                self.assertEqual(len(cases), 2)
                self.assertEqual(cases[0].environments[0].data, groups[0][0])
                self.assertEqual([env.stage for env in cases[1].environments], [6, 0, 1, 2, 3, 4])
                jobs = list(corpus.shader_jobs(cases[1]))
                self.assertEqual(len(jobs), 5)
                self.assertEqual(jobs[0][1].stage, 6)

    def test_real_dump_layout_and_dual_vertex_pairing(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = dump(Path(directory), ("va", "vb", "fs"))
            cases = list(corpus.dump_cases(manifest))
            self.assertEqual(len(cases), 1)
            self.assertEqual([env.stage for env in cases[0].environments], [6, 0, 4])
            jobs = list(corpus.shader_jobs(cases[0]))
            self.assertEqual(len(jobs), 2)
            self.assertEqual(jobs[0][1].stage, 6)

    def test_flat_legacy_dump_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pipeline = "0123456789abcdef"
            name = pipeline + "_s5_1111111111111111"
            (root / (name + ".maxwell.bin")).write_bytes(bytes(80) + struct.pack("<2Q", EXIT, 0))
            manifest = root / (pipeline + ".txt")
            manifest.write_text(
                f"pipeline={pipeline}\nstages=va,vb,tcs,tes,gs,fs\n{name} address=0 start=0 bytes=96\n"
            )
            self.assertEqual(next(iter(corpus.discover(root))).environments[0].stage, 4)

    def test_dump_sph_memory_fields_and_passthrough(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "shader.maxwell.bin"
            sph = bytearray(80)
            struct.pack_into("<3I", sph, 4, 128, 0, 32)
            path.write_bytes(sph + struct.pack("<2Q", EXIT, 0))
            env = corpus.dump_environment(path, 0, 0, 0, 96)
            self.assertEqual(corpus.HEADER.unpack_from(env.data)[5], 160)
            struct.pack_into("<I", sph, 0, 1 << 24)
            path.write_bytes(sph + struct.pack("<2Q", EXIT, 0))
            with self.assertRaisesRegex(ValueError, "passthrough mask"):
                corpus.dump_environment(path, 3, 0, 0, 96)

    def test_corrupt_cache_tail_preserves_prior_entries_and_other_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = cache(root / "a.bin", 15, [[environment()]])
            path.write_bytes(path.read_bytes() + struct.pack("<I", 1) + b"bad")
            cache(root / "b.bin", 18, [[environment(4)]])
            cases = list(corpus.discover(root))
            self.assertEqual(len(cases), 3)
            self.assertFalse(cases[0].reason)
            self.assertIn("truncated", cases[1].reason)
            self.assertFalse(cases[2].reason)

    def test_reject_invalid_header_counts_alignment_and_ranges(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, data in enumerate(
                (
                    corpus.MAGIC + struct.pack("<I", 99),
                    corpus.MAGIC + struct.pack("<2I", 15, 0),
                    corpus.MAGIC + struct.pack("<2I", 15, 7),
                    corpus.MAGIC
                    + struct.pack("<2I", 15, 1)
                    + corpus.HEADER.pack(7, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 5),
                    corpus.MAGIC
                    + struct.pack("<2I", 15, 1)
                    + corpus.HEADER.pack(16, corpus.MAX_MAP + 1, 0, 0, 0, 0, 2, 0, 0, 8, 1, 5),
                    corpus.MAGIC
                    + struct.pack("<2I", 15, 1)
                    + corpus.HEADER.pack(16, 0, 0, 0, 0, 0, 2, 0, 0xFFFFFFF8, 0, 1, 5),
                    corpus.MAGIC
                    + struct.pack("<2I", 15, 1)
                    + corpus.HEADER.pack(16, 0, 0, 0, 0, 0, 2, 0, 0, 8, 1, 7),
                )
            ):
                with self.subTest(index=index):
                    path = root / "opengl.bin"
                    path.write_bytes(data)
                    self.assertTrue(next(iter(corpus.discover(path))).reason)

    def test_reject_duplicate_map_and_mixed_stages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for group in (
                [environment(maps=[[(4, 2), (4, 3)], [], [], []])],
                [environment(5), environment(4)],
                [environment(4), environment(4)],
            ):
                path = cache(root / "bad.bin", 15, [group])
                self.assertTrue(next(iter(corpus.discover(path))).reason)

    def test_dump_failure_does_not_discard_other_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = dump(Path(directory))
            (manifest.parent / "vb.maxwell.bin").write_bytes(b"bad")
            cases = list(corpus.dump_cases(manifest))
            self.assertIn("byte count mismatch", cases[0].reason)
            self.assertEqual(cases[1].environments[0].stage, 4)

    def test_driver_cache_reports_no_guest_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vulkan_pipelines.bin"
            path.write_bytes(b"yuzuvkch" + struct.pack("<I", 18))
            self.assertIn("no guest", next(iter(corpus.discover(path))).reason)

    def test_integer_format_classification_matches_reference(self):
        reference = (
            (ROOT / "src/video_core/surface.cpp")
            .read_text()
            .split("bool IsPixelFormatInteger(", 1)[1]
            .split("bool IsPixelFormatSignedInteger(", 1)[0]
        )
        actual = (
            (TOOL / "environment.cpp")
            .read_text()
            .split("bool Environment::IsTexturePixelFormatInteger(", 1)[1]
            .split("std::optional<", 1)[0]
        )
        self.assertEqual(
            set(re.findall(r"case PixelFormat::(\w+)", reference)),
            set(re.findall(r"case F::(\w+)", actual)),
        )

    def test_absent_corpus_skips_without_requiring_compiler(self):
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()) as output:
            self.assertEqual(corpus.main(["--corpus", str(Path(directory) / "absent")]), 77)
            self.assertIn("SKIP: corpus absent", output.getvalue())

    def test_failure_timeout_signal_and_dxc_are_counted_and_continue(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(root / "data.bin", 15, [[environment()] for _ in range(5)])
            calls = 0

            def run(command, **kwargs):
                nonlocal calls
                if "-T" in command:
                    return subprocess.CompletedProcess(
                        command, 1, "", "sample:1:1: error: invalid HLSL"
                    )
                calls += 1
                if calls == 1:
                    return subprocess.CompletedProcess(
                        command, 1, "", "FAIL: unsupported opcode ImageAtomicIAdd32\n"
                    )
                if calls == 2:
                    raise subprocess.TimeoutExpired(command, 1)
                if calls == 3:
                    return subprocess.CompletedProcess(command, -6, "", "assertion failed")
                if calls == 4:
                    return subprocess.CompletedProcess(command, 0, "", "")
                Path(command[2]).write_text("float4 main() : SV_Target { return 0; }")
                return subprocess.CompletedProcess(command, 0, "", "")

            with patch.object(corpus.subprocess, "run", side_effect=run):
                report = corpus.evaluate(root, Path("emitter"), "dxc", timeout=1)
            self.assertEqual(calls, 5)
            self.assertEqual(
                report["counts"],
                {
                    "total": 5,
                    "emitted": 1,
                    "emitted_with_defaults": 0,
                    "failed": 5,
                    "dxc_passed": 0,
                    "dxc_failed": 1,
                },
            )
            self.assertIn("unsupported opcode ImageAtomicIAdd32", report["failures_by_reason"])
            self.assertEqual(report["failures_by_opcode"], {"ImageAtomicIAdd32": 1})
            self.assertTrue(any("signal 6" in reason for reason in report["failures_by_reason"]))

    def test_defaults_and_both_dxc_profiles_with_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(root / "data.bin", 15, [[environment()]])
            commands = []

            def run(command, **kwargs):
                commands.append(command)
                if "-T" in command:
                    Path(command[command.index("-Fo") + 1]).write_bytes(b"DXIL")
                    return subprocess.CompletedProcess(command, 0, "", "")
                Path(command[2]).write_text("void main() {}")
                return subprocess.CompletedProcess(
                    command, 0, "DEFAULT: stage texture format key=7\n" * 2, ""
                )

            for disabled in (False, True):
                commands.clear()
                with patch.object(corpus.subprocess, "run", side_effect=run):
                    report = corpus.evaluate(
                        root, Path("emitter"), "dxc", sm66=True, dxc_no_validation=disabled
                    )
                row = report["rows"][0]
                self.assertEqual(row["status"], "emitted_with_defaults")
                self.assertEqual(row["synthesized_keys"], ["stage texture format key=7"])
                self.assertEqual(report["counts"]["emitted_with_defaults"], 1)
                self.assertEqual(report["counts"]["dxc_passed"], 1)
                self.assertEqual([a["profile"] for a in row["dxc_attempts"]], ["cs_6_0", "cs_6_6"])
                self.assertEqual(report["dxc_profiles"]["cs_6_6"]["passed"], 1)
                for command in commands[1:]:
                    self.assertEqual("-Vd" in command, disabled)

    def test_failed_sm60_still_attempts_sm66(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(root / "data.bin", 15, [[environment()]])

            def run(command, **kwargs):
                if "cs_6_0" in command:
                    return subprocess.CompletedProcess(command, 1, "", "error: SM 6.6 required")
                if "cs_6_6" in command:
                    Path(command[command.index("-Fo") + 1]).write_bytes(b"DXIL")
                else:
                    Path(command[2]).write_text("void main() {}")
                return subprocess.CompletedProcess(command, 0, "", "")

            with patch.object(corpus.subprocess, "run", side_effect=run):
                report = corpus.evaluate(root, Path("emitter"), "dxc", sm66=True)
            self.assertEqual(report["counts"]["dxc_failed"], 1)
            self.assertEqual(report["counts"]["failed"], 1)
            self.assertEqual(report["dxc_profiles"]["cs_6_6"]["passed"], 1)


class ShaderCorpusCompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("NXBOX_SHADER_CORPUS_EMITTER")
        if not configured:
            raise unittest.SkipTest(
                "set NXBOX_SHADER_CORPUS_EMITTER to the built host tool for compiler integration"
            )
        cls.emitter = Path(configured).resolve()
        if not cls.emitter.is_file():
            raise AssertionError(f"configured corpus compiler does not exist: {cls.emitter}")

    def test_real_environment_defaults_preserve_captured_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "environment.bin"
            path.write_bytes(environment(maps=[[(4, 4)], [(4, 3)], [((2 << 32) | 16, 123)], []]))
            probe = self.emitter.with_name("nxbox-corpus-environment-test" + self.emitter.suffix)
            result = subprocess.run(
                [str(probe), str(path)], capture_output=True, text=True, timeout=30, check=False
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_cache_abi_sizes(self):
        result = subprocess.run(
            [str(self.emitter), "--layout"], check=False, capture_output=True, text=True, timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "gl=624 vk=1056")

    def test_synthetic_guest_corpus_real_maxwell_ir_hlsl(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(root / "opengl.bin", 15, [[environment(0)], [environment(4)], [environment(5)]])
            cache(root / "vulkan.bin", 18, [[environment(0)], [environment(4)], [environment(5)]])
            dump(root / "dump")
            report = corpus.evaluate(root, self.emitter)
            self.assertEqual(report["counts"]["total"], 8)
            self.assertEqual(report["counts"]["emitted"], 8, json.dumps(report, indent=2))
            self.assertEqual(report["counts"]["failed"], 0, json.dumps(report, indent=2))

    def test_synthetic_missing_texture_and_cbuf_emit_and_compile_with_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sph = bytearray(80)
            struct.pack_into("<I", sph, 72, 1)  # Live R0 fragment color output.
            # TEX 2D, coordinate RZ, mask X, sparse predicate PT, bound cbuf offset 0.
            tex = 0xC000000000070000 | (255 << 8) | (2 << 28) | (1 << 31) | (7 << 51)
            code = bytes(sph) + struct.pack("<2Q", tex, EXIT)
            cache(root / "defaults.bin", 15, [[environment(4, code=code, sph=bytes(sph))]])
            dxc = shutil.which(os.environ.get("DXC", "dxc"))
            report = corpus.evaluate(root, self.emitter, dxc, sm66=True)
            self.assertEqual(report["counts"]["failed"], 0, report["rows"])
            self.assertEqual(report["counts"]["emitted_with_defaults"], 1)
            self.assertEqual(
                report["rows"][0]["synthesized_keys"],
                [
                    "stage constant buffer key=8589934592",
                    "stage texture format key=0",
                    "stage texture type key=0",
                ],
            )
            if dxc:
                self.assertEqual(
                    report["dxc_profiles"],
                    {
                        "ps_6_0": {"passed": 1, "failed": 0},
                        "ps_6_6": {"passed": 1, "failed": 0},
                    },
                )

    def test_fragment_varying_has_an_isolated_input_signature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sph = bytearray(80)
            sph[24] = 3  # Generic0X uses screen-linear interpolation.
            struct.pack_into("<I", sph, 72, 1)  # Fragment R0 is a live color output.
            ipa = 0xE000000000070000 | (255 << 8) | (32 << 30)
            code = bytes(sph) + struct.pack("<2Q", ipa, EXIT)
            cache(root / "varying.bin", 15, [[environment(4, code=code, sph=bytes(sph))]])
            output = root / "output"
            report = corpus.evaluate(root, self.emitter, output=output)
            self.assertEqual(report["counts"]["emitted"], 1, report["rows"])
            self.assertEqual(report["counts"]["failed"], 0, report["rows"])
            source = Path(report["rows"][0]["source"]).read_text()
            self.assertIn("float4 attr0 : TEXCOORD0", source)
            self.assertIn("input.attr0.x", source)

    def test_all_guest_stages_and_real_dual_vertex_merge(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(
                root / "all.bin",
                18,
                [[environment(stage) for stage in (6, 0, 1, 2, 3, 4)], [environment(5)]],
            )
            report = corpus.evaluate(root, self.emitter)
            self.assertEqual(report["counts"]["total"], 6)
            self.assertEqual(set(report["stages"]), {"vb", "tcs", "tes", "gs", "fs", "cs"})
            self.assertTrue(report["rows"][0]["emitted"], report["rows"][0])
            self.assertGreaterEqual(report["counts"]["emitted"], 3)
            self.assertEqual(report["counts"]["total"], len(report["rows"]))

    def test_real_worker_failure_preserves_following_shader(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # EXIT is conditional on PT=false, so decoding runs outside the capture.
            cache(root / "bad.bin", 15, [[environment(code=bytes(16))], [environment()]])
            report = corpus.evaluate(root, self.emitter)
            self.assertEqual(report["counts"]["total"], 2)
            self.assertEqual(report["counts"]["emitted"], 1)
            self.assertEqual(report["counts"]["failed"], 1)
            self.assertIn("shader recompiler assertion failed", report["rows"][0]["reason"])

    def test_cli_json_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(root / "opengl.bin", 15, [[environment()]])
            path = root / "report.json"
            with redirect_stdout(io.StringIO()) as output:
                result = corpus.main(
                    ["--corpus", str(root), "--emitter", str(self.emitter), "--report", str(path)]
                )
            self.assertEqual(result, 0, output.getvalue())
            self.assertEqual(json.loads(path.read_text())["counts"]["emitted"], 1)
            self.assertIn("| total | emitted | failed |", output.getvalue())

    def test_synthetic_guest_corpus_dxc(self):
        dxc = shutil.which(os.environ.get("DXC", "dxc"))
        if not dxc:
            self.skipTest("DXC unavailable; emission is tested separately")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache(root / "opengl.bin", 15, [[environment(0)], [environment(4)], [environment(5)]])
            report = corpus.evaluate(root, self.emitter, dxc)
            self.assertEqual(report["counts"]["dxc_passed"], 3, json.dumps(report, indent=2))
            self.assertEqual(report["counts"]["failed"], 0, json.dumps(report, indent=2))

    def test_user_corpus(self):
        path = Path(
            os.environ.get("NXBOX_SHADER_CORPUS", "~/.local/share/nxbox/corpus")
        ).expanduser()
        if not path.exists():
            self.skipTest(f"corpus absent: {path}")
        report = corpus.evaluate(path, self.emitter, shutil.which(os.environ.get("DXC", "dxc")))
        if not report["counts"]["total"]:
            self.skipTest("no recognized user corpus inputs")
        counts = report["counts"]
        self.assertEqual(counts["total"], len(report["rows"]))
        self.assertEqual(counts["emitted"], sum(row["emitted"] for row in report["rows"]))
        self.assertEqual(counts["failed"], sum(bool(row["reason"]) for row in report["rows"]))
        print("User corpus:", json.dumps(counts, sort_keys=True))


if __name__ == "__main__":
    unittest.main()
