# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise completion reentry and resource ownership without a graphics SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <memory>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_completion.h"
int main() {
    std::vector<std::function<void()>> callbacks;
    std::vector<std::shared_ptr<int>> resources;
    auto old_resource = std::make_shared<int>(1);
    std::weak_ptr<int> old = old_resource;
    resources.push_back(std::move(old_resource));
    std::weak_ptr<int> next;
    std::vector<int> order;
    callbacks.push_back([&] {
        order.push_back(1);
        assert(!old.expired());
        // A recursive wait must see an empty batch, not invoke this callback again.
        D3D12::CompleteBatch(callbacks, resources);
        assert(!old.expired());
        auto next_resource = std::make_shared<int>(2);
        next = next_resource;
        resources.push_back(std::move(next_resource));
        // Force repeated reallocations of the pending callback vector.
        for (int i = 0; i < 512; ++i)
            callbacks.push_back([&, i] { order.push_back(i + 3); });
    });
    callbacks.push_back([&] { order.push_back(2); assert(!old.expired()); });
    D3D12::CompleteBatch(callbacks, resources);
    assert((order == std::vector<int>{1, 2}));
    assert(old.expired() && !next.expired());
    assert(callbacks.size() == 512 && resources.size() == 1);
    D3D12::CompleteBatch(callbacks, resources);
    assert(order.size() == 514 && order.back() == 514);
    assert(next.expired() && callbacks.empty() && resources.empty());
    // A throwing callback cannot leave itself queued for a later teardown wait.
    callbacks.push_back([] { throw std::runtime_error("callback failure"); });
    try { D3D12::CompleteBatch(callbacks, resources); assert(false); }
    catch (const std::runtime_error&) {}
    assert(callbacks.empty());
    D3D12::CompleteBatch(callbacks, resources);
}
"""


class D3D12CompletionTests(unittest.TestCase):
    def test_reentrant_completion_and_resource_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-completion-") as temp:
            source = Path(temp) / "completion.cpp"
            binary = Path(temp) / "completion"
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
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)
