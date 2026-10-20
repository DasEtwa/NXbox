# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the UWP Auto emission policy with the real Dynarmic configuration."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class JitMemoryContextPolicy(unittest.TestCase):
    def test_auto_default_opt_out_and_fault_exit_safety(self):
        program = r"""
#include <cassert>
#include <cstdlib>
#include "dynarmic/interface/A64/config.h"
#include "core/arm/dynarmic/nxbox_jit_policy.h"
using Core::NxboxJit::ApplyA64OptOuts;
int main() {
    Dynarmic::A64::UserConfig config{};
    const auto flags = config.optimizations;
    config.fastmem_address_space_bits = 39;
    ApplyA64OptOuts(config, 39, true);
    assert(!config.collect_memory_access_context);
    assert(config.optimizations == flags);
    assert(!config.fastmem_pointer && !config.fastmem_exclusive_access);
    assert(config.fastmem_address_space_bits == 39);
    assert(!config.check_halt_on_memory_access);
    assert(!config.unsafe_optimizations);

    // Accurate/debug/null-default callers retain the old diagnostic behavior.
    ApplyA64OptOuts(config, 39);
    assert(config.collect_memory_access_context);
    // Only exact zero disables the new fast path, like the other JIT switches.
    for (const auto* value : {"0", "1", "false", "", "00"}) {
        setenv("NXBOX_JIT_FAST_MEMORY_CONTEXT", value, 1);
        ApplyA64OptOuts(config, 39, true);
        assert(config.collect_memory_access_context == (value[0] == '0' && value[1] == '\0'));
    }
    unsetenv("NXBOX_JIT_FAST_MEMORY_CONTEXT");
    setenv("NXBOX_JIT_PRECISE_MEMORY", "1", 1);
    ApplyA64OptOuts(config, 39, true);
    assert(config.collect_memory_access_context);
    // A protection fault exit needs live PC/register context, even with legacy
    // diagnostics disabled explicitly. Never sacrifice watchpoint correctness.
    setenv("NXBOX_JIT_PRECISE_MEMORY", "0", 1);
    config.check_halt_on_memory_access = true;
    ApplyA64OptOuts(config, 39, true);
    assert(config.collect_memory_access_context);
    ApplyA64OptOuts(config, 39, false);
    assert(config.collect_memory_access_context);
    config.check_halt_on_memory_access = false;
    ApplyA64OptOuts(config, 39, true);
    assert(!config.collect_memory_access_context);
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-jit-context-") as directory:
            source = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            source.write_text(program)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'src'}",
                    f"-I{ROOT / 'src/dynarmic/src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            environment = {
                key: value for key, value in os.environ.items() if not key.startswith("NXBOX_JIT_")
            }
            subprocess.run([str(binary)], env=environment, check=True, timeout=10)

    def test_runtime_wiring_only_allows_auto_without_debugger(self):
        source = (ROOT / "src/core/arm/dynarmic/arm_dynarmic_64.cpp").read_text()
        policy_call = source.split("NxboxJit::ApplyA64OptOuts(", 1)[1].split(";", 1)[0]
        self.assertIn("Settings::CpuAccuracy::Auto", policy_call)
        self.assertIn("!m_system.DebuggerEnabled()", policy_call)
        self.assertIn("#ifdef NXBOX_UWP", source[: source.index(policy_call)])


if __name__ == "__main__":
    unittest.main()
