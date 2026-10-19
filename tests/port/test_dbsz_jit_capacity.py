# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""The larger A64 cache is bounded and opt-in on memory-constrained Xbox builds."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class DbszJitCapacity(unittest.TestCase):
    def test_capacity_switch_is_exact_latched_and_bounded(self):
        driver = r"""
#include <cassert>
#include <cstdlib>
#include "core/arm/dynarmic/nxbox_jit_policy.h"
int main(int argc, char**) {
    constexpr unsigned MiB = 1024 * 1024;
    const auto capacity = Core::NxboxJit::UwpCodeCacheSize();
    assert(capacity == (argc > 1 ? 192u : 128u) * MiB);
    // Four cores add at most 256 MiB; do not adopt desktop's 2 GiB total budget.
    assert(capacity * 4 <= 768u * MiB);
    // Captured core-0 high-water mark before its capacity flush needs >128 MiB.
    const auto required = 130748812u + MiB + 4u * MiB;
    assert((required < capacity) == (argc > 1));
    setenv("NXBOX_JIT_LARGE_CACHE", argc > 1 ? "0" : "1", 1);
    assert(Core::NxboxJit::UwpCodeCacheSize() == capacity);
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-dbsz-jit-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(driver)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    "-I",
                    str(ROOT / "src/dynarmic/src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            for value in [None, "0", "1", "true", "11", "", "2"]:
                env = dict(os.environ)
                env.pop("NXBOX_JIT_LARGE_CACHE", None)
                if value is not None:
                    env["NXBOX_JIT_LARGE_CACHE"] = value
                arguments = [str(binary)] + (["large"] if value == "1" else [])
                subprocess.run(arguments, env=env, check=True, timeout=10)

    def test_only_uwp_a64_uses_the_policy_and_null_jit_stays_small(self):
        source = (ROOT / "src/core/arm/dynarmic/arm_dynarmic_64.cpp").read_text()
        self.assertIn(
            "#if defined(NXBOX_UWP)\n    config.code_cache_size = NxboxJit::UwpCodeCacheSize();",
            source,
        )
        self.assertLess(
            source.index("NxboxJit::UwpCodeCacheSize()"), source.index("if (!page_table)")
        )
        null_jit = source[source.index("if (!page_table)") :]
        self.assertIn("config.code_cache_size = std::uint32_t(8_MiB);", null_jit)
        self.assertIn("config.code_cache_size);", source)


if __name__ == "__main__":
    unittest.main()
