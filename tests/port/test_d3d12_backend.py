# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native renderer selection without Windows or graphics dependencies."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <string>
#include "video_core/renderer_d3d12/backend_selection.h"
int main() {
    // A valid environment override wins for every ASCII case combination.
    for (unsigned mask = 0; mask < 32; ++mask) {
        std::string value = "d3d12";
        for (unsigned i = 0; i < value.size(); ++i) {
            if ((mask & (1u << i)) && value[i] >= 'a' && value[i] <= 'z')
                value[i] -= 'a' - 'A';
        }
        assert(D3D12::UseNativeBackend(false, value));
        assert(D3D12::UseNativeBackend(true, value));
    }
    // Missing, unknown and malformed overrides cannot silently change configured backends.
    for (const auto value : {"", "opengl", "vulkan", "null", "d3d11", "d3d12x", " d3d12",
                             "d3d12 ", "d3d1", "d3d\xFF"}) {
        assert(!D3D12::UseNativeBackend(false, value));
        assert(D3D12::UseNativeBackend(true, value));
    }
}
"""


class D3D12BackendTests(unittest.TestCase):
    def test_selection_precedence(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-selection-") as temp:
            source = Path(temp) / "selection.cpp"
            binary = Path(temp) / "selection"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_persisted_backend_numbers_remain_stable(self):
        source = (ROOT / "src/common/settings_enums.h").read_text()
        values = re.search(r"ENUM\(RendererBackend,\s*([^;]+)\);", source).group(1)
        self.assertEqual(
            [value.strip() for value in values.split(",")],
            ["OpenGL_GLSL", "Vulkan", "Null", "OpenGL_GLASM", "OpenGL_SPIRV", "D3D12"],
        )
