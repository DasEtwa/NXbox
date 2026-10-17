# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline capture parsing, diagnostics, and optional real HLSL/DXC compilation."""

import argparse
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/hlsl-harness"
spec = importlib.util.spec_from_file_location("hlsl_harness", HARNESS / "run.py")
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


class HlslHarnessTests(unittest.TestCase):
    def test_dump_pairing_and_length_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            pipeline = "0123456789abcdef"
            names = [f"{pipeline}_s{i}_100000000000000{i}" for i in (0, 1, 5)]
            manifest = folder / (pipeline + ".txt")
            manifest.write_text(
                f"pipeline={pipeline}\nstages=va,vb,tcs,tes,gs,fs\n"
                + "".join(f"{name} address=4096 start=4096 bytes=96\n" for name in names)
            )
            for name in names:
                (folder / (name + ".maxwell.bin")).write_bytes(bytes(96))
            cases = harness.dump_cases(folder)
            self.assertEqual([case[1] for case in cases], ["vs", "ps"])
            self.assertEqual(
                cases[0][3], [str(folder / (names[0] + ".maxwell.bin")), "4096", "4096"]
            )
            (folder / (names[1] + ".maxwell.bin")).write_bytes(bytes(88))
            with self.assertRaisesRegex(ValueError, "byte count mismatch"):
                harness.dump_cases(folder)
            manifest.write_text("bad stage record\n")
            with self.assertRaisesRegex(ValueError, "invalid stage record"):
                harness.dump_cases(folder)

    def test_diagnostic_source_context(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "bad.hlsl"
            source.write_text("\n".join(f"line {i}" for i in range(1, 10)))
            output = io.StringIO()
            with redirect_stdout(output):
                harness.show_lines(source, f"{source}:5:3: error: unknown symbol")
            self.assertIn("    5 | line 5", output.getvalue())
            self.assertNotIn("line 1", output.getvalue())
            output = io.StringIO()
            with redirect_stdout(output):
                harness.show_lines(source, "DXC failed without a source location")
            self.assertIn("    1 | line 1", output.getvalue())
            self.assertIn("    9 | line 9", output.getvalue())

    def test_compile_failure_reports_lines_and_continues(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "src/shader_recompiler/backend/hlsl/emit_hlsl.h"
            header.parent.mkdir(parents=True)
            header.touch()
            results = root / "results"
            results.mkdir()
            (results / "synthetic_vs.dxil").write_bytes(b"stale")
            args = argparse.Namespace(
                dxc=sys.executable,
                emitter=Path(sys.executable),
                dumps=None,
                output=results,
                timeout=60,
            )
            targets = []

            def execute(command, **kwargs):
                if command[1] == "--probe":
                    return subprocess.CompletedProcess(command, 0, "available", "")
                if command[1] == "--synthetic":
                    Path(command[-1]).write_text("float4 main() {\n  broken;\n}\n")
                    return subprocess.CompletedProcess(command, 0, "", "")
                targets.append(command[2])
                if command[2] == "vs_6_0":
                    return subprocess.CompletedProcess(
                        command, 1, "", f"{command[-1]}:2:3: error: broken symbol"
                    )
                Path(command[6]).write_bytes(b"test artifact")
                return subprocess.CompletedProcess(command, 0, "", "")

            output = io.StringIO()
            with (
                patch.object(harness, "ROOT", root),
                patch.object(harness.subprocess, "run", side_effect=execute),
                redirect_stdout(output),
            ):
                self.assertEqual(harness.validate(args), 1)
            self.assertEqual(targets, ["vs_6_0", "ps_6_0", "cs_6_0"])
            self.assertIn("    2 |   broken;", output.getvalue())
            self.assertIn("2 passed, 1 failed", output.getvalue())
            self.assertFalse((results / "synthetic_vs.dxil").exists())
            self.assertIn("broken symbol", (results / "synthetic_vs.dxc.log").read_text())

    def test_backend_absence_is_explicit_skip(self):
        if (ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl.h").exists():
            self.skipTest("backend has landed")
        result = subprocess.run(
            [sys.executable, str(HARNESS / "run.py")], check=False, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 77)
        self.assertIn("SKIP:", result.stdout)
        self.assertIn("backend/hlsl", result.stdout)

    def test_synthetic_ir_with_dxc(self):
        dxc = shutil.which(os.environ.get("DXC", "dxc"))
        if not dxc:
            self.skipTest("DXC unavailable; run tools/hlsl-harness/fetch_dxc.py")
        if not (ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl.h").exists():
            self.skipTest("backend/hlsl is not available yet")
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            configured_emitter = os.environ.get("NXBOX_HLSL_EMITTER")
            if configured_emitter:
                emitter = Path(configured_emitter)
            else:
                cmake = shutil.which("cmake")
                if not cmake:
                    self.skipTest("CMake unavailable for building hlsl-emitter")
                command = [cmake, "-S", str(HARNESS), "-B", str(build)]
                if os.environ.get("BOOST_ROOT"):
                    command.append("-DBOOST_ROOT=" + os.environ["BOOST_ROOT"])
                configured = subprocess.run(
                    command, check=False, capture_output=True, text=True, timeout=180
                )
                # Dependency absence is a skip; compiler/configuration errors are failures.
                if configured.returncode and (
                    "Could NOT find Boost" in configured.stdout + configured.stderr
                ):
                    self.skipTest("Boost >=1.81 headers unavailable")
                self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                compiled = subprocess.run(
                    [cmake, "--build", str(build), "--target", "hlsl-emitter", "--parallel", "2"],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=600,
                )
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                emitter = build / "hlsl-emitter"
            result = subprocess.run(
                [
                    sys.executable,
                    str(HARNESS / "run.py"),
                    "--dxc",
                    dxc,
                    "--emitter",
                    str(emitter),
                    "--output",
                    str(Path(directory) / "results"),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=240,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("3 passed, 0 failed", result.stdout)
            self.assertEqual(len(list((Path(directory) / "results").glob("*.dxil"))), 3)


if __name__ == "__main__":
    unittest.main()
