# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-level checks for the JIT emit sub-phase timers and the dual-mapping probe."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
HEADER = (ROOT / "src/common/nxbox_stall.h").read_text()
PROBE = (ROOT / "src/eden_uwp/jit_dualmap_probe.cpp").read_text()

PHASES = {
    "JitEmitInsts": "jitemitinsts",
    "JitEmitCapture": "jitemitcapture",
    "JitEmitTerminal": "jitemitterm",
    "JitEmitRanges": "jitemitranges",
    "JitEmitRegister": "jitemitregister",
    "JitEmitPatch": "jitemitpatch",
    "JitEmitWx": "jitemitwx",
    "JitEmitCommit": "jitemitcommit",
}


class JitEmitPhaseTests(unittest.TestCase):
    def test_phases_are_kinds_and_in_the_game_stall_table(self):
        total = int(re.search(r"std::array<std::pair<NxboxStall::Kind, const char\*>, (\d+)> stall_kinds", SESSION)[1])
        entries = re.findall(r'\{NxboxStall::Kind::(\w+), "(\w+)"\}', SESSION)
        self.assertEqual(total, len(entries))
        for kind, name in PHASES.items():
            self.assertIn(f"    {kind},", HEADER)
            self.assertIn((kind, name), entries)

    def test_every_phase_has_a_scope(self):
        sources = "".join(
            (ROOT / "src/dynarmic/src/dynarmic/backend/x64" / f).read_text()
            for f in ("a64_emit_x64.cpp", "emit_x64.cpp", "block_of_code.cpp")
        )
        for kind in PHASES:
            self.assertIn(f"NxboxStall::Scope", sources)
            self.assertIn(f"Kind::{kind}}}", sources)


class DualMapProbeTests(unittest.TestCase):
    def test_log_line_has_every_field(self):
        for field in ("create=", "rw=", "rx=", "exec1=", "exec2=", "alias_us=", "protect_us="):
            self.assertIn(field, PROBE)
        self.assertIn("JIT_DUALMAP_PROBE", PROBE)

    def test_probe_is_seh_wrapped(self):
        self.assertIn("__try", PROBE)
        self.assertIn("__except", PROBE)
        self.assertIn("catch (...)", PROBE)

    def test_probe_only_runs_behind_the_env_var(self):
        calls = [m.start() for m in re.finditer(r"NxboxJitDualMapProbe\(\);", SESSION)]
        self.assertEqual(len(calls), 1)
        guard = SESSION.rfind("NXBOX_JIT_DUALMAP_PROBE", 0, calls[0])
        self.assertGreater(guard, calls[0] - 300)
        self.assertIn("jit_dualmap_probe.cpp", (ROOT / "src/eden_uwp/CMakeLists.txt").read_text())


if __name__ == "__main__":
    unittest.main()
