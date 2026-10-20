# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production constant-remap fill's cache ownership handoff."""

import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run

ROOT = Path(__file__).resolve().parents[2]


class MaxwellDmaClearTests(unittest.TestCase):
    def test_declined_acceleration_invalidates_guest_memory_caches(self):
        source = (ROOT / "src/video_core/engines/maxwell_dma.cpp").read_text()
        start = source.index("            const u32 component_size =")
        end = source.index("\n        } else {", start)
        body = source[start:end]
        fixture = (ROOT / "tests/port/fixtures/maxwell_dma_clear.cpp").read_text()
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "fmt"], text=True))
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                fixture.replace("// PRODUCTION_BODY", body),
                Path(directory),
                extra_flags=[*flags, "-fsanitize=address,undefined", "-Wall", "-Wextra"],
            )


if __name__ == "__main__":
    unittest.main()
