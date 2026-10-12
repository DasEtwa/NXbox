# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify failure transport across callbacks and geometry bounds without a GPU SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>
#include "common/deferred_exception.h"
#include "video_core/renderer_d3d12/d3d12_buffer_view.h"
int main() {
    Common::DeferredException failure;
    int calls = 0;
    failure.Invoke([&] { ++calls; });
    assert(!failure.HasError());
    failure.Invoke([] { throw std::runtime_error("HRESULT=0x887a0005"); });
    assert(failure.HasError());
    failure.Invoke([&] { ++calls; });
    assert(calls == 1);
    try { failure.Rethrow(); assert(false); }
    catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "HRESULT=0x887a0005");
    }
    assert(!failure.HasError());
    failure.Rethrow();
    failure.Invoke([] { throw 42; });
    try { failure.Rethrow(); assert(false); }
    catch (int value) { assert(value == 42); }
    D3D12::ValidateBufferView(4096, 256, 128, 128);
    const auto invalid = [](auto address, auto capacity, auto offset, auto size) {
        try { D3D12::ValidateBufferView(address, capacity, offset, size); assert(false); }
        catch (const std::out_of_range&) {}
    };
    invalid(0, 256, 0, 128);
    invalid(4096, 256, 0, 0);
    invalid(4096, 256, 257, 1);
    invalid(4096, 256, 128, 129);
    invalid(std::numeric_limits<std::uint64_t>::max() - 1, 256, 2, 1);
    invalid(std::numeric_limits<std::uint64_t>::max() - 1, 256, 0, 2);
}
"""


class D3D12FailureTests(unittest.TestCase):
    def test_callback_failure_and_geometry_bounds(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-failure-") as temp:
            source = Path(temp) / "failure.cpp"
            binary = Path(temp) / "failure"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
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
            )
            subprocess.run([str(binary)], check=True)
