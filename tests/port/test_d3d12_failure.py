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
#include <future>
#include <chrono>
#include "common/cancellable_sync_queue.h"
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
    Common::CancellableSyncQueue queue;
    const auto first = queue.Request([&] { ++calls; });
    queue.Tick();
    queue.Wait(first);
    assert(queue.Completed() == first && calls == 2);
    const auto failed = queue.Request([] { throw std::runtime_error("GPU failure"); });
    const auto pending = queue.Request([&] { ++calls; });
    auto waiter = std::async(std::launch::async, [&] { queue.Wait(pending); });
    assert(waiter.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    try { queue.Tick(); assert(false); }
    catch (const std::runtime_error&) {}
    // Cancellation can acquire the lock after a callback throws, wakes the CPU,
    // and does not execute either pending or subsequently requested GPU work.
    queue.Cancel();
    assert(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();
    queue.Wait(failed);
    const auto after_failure = queue.Request([&] { ++calls; });
    queue.Wait(after_failure);
    queue.Tick();
    assert(calls == 2 && queue.Completed() == after_failure);
    Common::CancellableSyncQueue reentrant;
    reentrant.Request([&] { reentrant.Request([&] { ++calls; }); });
    reentrant.Tick();
    assert(calls == 3 && reentrant.Completed() == 2);
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
