# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and execute the emitter's source reduction unit tests."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class HlslSourceTests(unittest.TestCase):
    def test_source_reduction(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("A host C++ compiler is required")
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-source-") as directory:
            binary = Path(directory) / "hlsl-source-tests"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{root / 'src'}",
                    str(root / "tests/port/hlsl_source.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


class SpirvAuditTests(unittest.TestCase):
    def setUp(self):
        import sys

        tool = Path(__file__).resolve().parents[2] / "tools/nxbox/shader_corpus"
        if str(tool) not in sys.path:
            sys.path.insert(0, str(tool))
        from check_size import canonical_ids
        from spirv_audit import graph, phi_normalized_graph

        self.canonical = canonical_ids
        self.graph = graph
        self.phi_graph = phi_normalized_graph
        self.source = """OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %8 "main" %7
OpDecorate %7 Location 0
%1 = OpTypeVoid
%2 = OpTypeFunction %1
%3 = OpTypeFloat 32
%4 = OpConstant %3 1
%5 = OpConstant %3 2
%6 = OpTypePointer Output %3
%7 = OpVariable %6 Output
%8 = OpFunction %1 None %2
%9 = OpLabel
%10 = OpFAdd %3 %4 %5
OpStore %7 %10
OpReturn
OpFunctionEnd
"""

    def test_compiler_cache_tracks_source_content(self):
        import hashlib
        import json
        from unittest.mock import patch

        from check_size import compile_shader

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "shader.hlsl"
            cache = root / "cache"
            cache.mkdir()
            source.write_text("original")
            status = {
                "ok": False,
                "phase": 0,
                "diagnostics": "old failure",
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            }
            (cache / "status.json").write_text(json.dumps(status))
            with patch("check_size.subprocess.run") as run:
                self.assertEqual(compile_shader(source, "fs", cache), status)
                run.assert_not_called()
                source.write_text("changed")
                run.return_value = subprocess.CompletedProcess([], 1, "new failure", "")
                result = compile_shader(source, "fs", cache)
                run.assert_called_once()
                self.assertEqual(result["diagnostics"], "new failure")
                self.assertNotEqual(result["source_sha256"], status["source_sha256"])

    def test_numbering_and_global_order(self):
        import re

        renamed = re.sub(r"%\d+", lambda m: f"%{int(m[0][1:]) + 100}", self.source)
        renamed = renamed.replace("%104 = OpConstant %103 1\n", "")
        renamed = renamed.replace(
            "%107 = OpVariable", "%104 = OpConstant %103 1\n%107 = OpVariable"
        )
        self.assertEqual(self.canonical(self.source), self.canonical(renamed))
        self.assertEqual(self.graph(self.source), self.graph(renamed))

    def test_commutativity_requires_explicit_opt_in(self):
        swapped = self.source.replace("OpFAdd %3 %4 %5", "OpFAdd %3 %5 %4")
        self.assertNotEqual(self.graph(self.source), self.graph(swapped))
        self.assertEqual(self.graph(self.source, commute=True), self.graph(swapped, commute=True))
        subtracted = self.source.replace("OpFAdd", "OpFSub")
        self.assertNotEqual(
            self.graph(self.source, commute=True), self.graph(subtracted, commute=True)
        )

    def test_phi_declaration_order_preserves_incoming_edges(self):
        phis = self.source.replace(
            "%10 = OpFAdd %3 %4 %5",
            "%11 = OpPhi %3 %4 %9 %5 %9\n%12 = OpPhi %3 %5 %9 %4 %9\n%10 = OpFAdd %3 %11 %4",
        )
        reordered = phis.replace(
            "%11 = OpPhi %3 %4 %9 %5 %9\n%12 = OpPhi %3 %5 %9 %4 %9",
            "%12 = OpPhi %3 %5 %9 %4 %9\n%11 = OpPhi %3 %4 %9 %5 %9",
        )
        self.assertEqual(self.phi_graph(phis), self.phi_graph(reordered))
        changed = phis.replace("OpFAdd %3 %11 %4", "OpFAdd %3 %12 %4")
        self.assertNotEqual(self.phi_graph(phis), self.phi_graph(changed))

    def test_precision_values_bindings_and_execution_mode_are_significant(self):
        changes = [
            self.source.replace("OpConstant %3 2", "OpConstant %3 3"),
            self.source.replace("OpDecorate %7 Location 0", "OpDecorate %7 Location 1"),
            self.source.replace("%10 = OpFAdd", "OpDecorate %10 NoContraction\n%10 = OpFAdd"),
            self.source.replace(
                "OpMemoryModel", "OpExecutionMode %8 EarlyFragmentTests\nOpMemoryModel"
            ),
        ]
        for changed in changes:
            self.assertNotEqual(self.canonical(self.source), self.canonical(changed))
            self.assertNotEqual(
                self.graph(self.source, commute=True, indices=True),
                self.graph(changed, commute=True, indices=True),
            )


if __name__ == "__main__":
    unittest.main()
