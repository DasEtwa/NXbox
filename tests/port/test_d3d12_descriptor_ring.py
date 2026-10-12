# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the descriptor ring allocator math on the host (no Windows SDK)."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
using D3D12::RingAllocator;
int main() {
    // Preflight all heaps before reserving any tables. A full sampler heap must
    // force retirement before resource descriptors are tagged for the next draw.
    RingAllocator views{8}, samplers{1};
    assert(samplers.Allocate(1, 1) == 0u);
    assert(views.CanAllocate(4) && !samplers.CanAllocate(1));
    assert(views.Used() == 0 && !views.OldestTick());
    samplers.Retire(1);
    assert(views.CanAllocate(4) && samplers.CanAllocate(1));
    assert(views.Allocate(4, 2) == 0u);
    assert(samplers.Allocate(1, 2) == 0u);
    views.Retire(1); samplers.Retire(1);
    assert(views.Used() == 4 && samplers.Used() == 1);
    assert(!views.CanAllocate(0) && !views.CanAllocate(9));
    RingAllocator ring{10};
    // Contiguous allocations fill the heap in order.
    assert(ring.Allocate(4, 1) == 0u);
    assert(ring.Allocate(4, 1) == 4u);
    // 2 left at the end: a 3-wide request does not fit, nothing is free at the front yet.
    assert(!ring.Allocate(3, 2));
    assert(ring.Allocate(2, 2) == 8u);
    assert(ring.Used() == 10);
    assert(!ring.Allocate(1, 2));  // full
    assert(*ring.OldestTick() == 1);
    // Retiring tick 1 frees [0, 8).
    ring.Retire(1);
    assert(ring.Used() == 2);
    // End has 0 free, so the request wraps to 0.
    assert(ring.Allocate(5, 3) == 0u);
    // Remaining free region is [5, 8).
    assert(!ring.Allocate(4, 3));
    assert(ring.Allocate(3, 3) == 5u);
    assert(ring.Used() == 10);
    ring.Retire(2);  // frees the tick-2 slot at 8..10
    assert(ring.Used() == 8);
    ring.Retire(3);
    assert(ring.Used() == 0 && !ring.OldestTick());
    // Empty ring restarts at 0.
    assert(ring.Allocate(10, 4) == 0u);
    assert(!ring.Allocate(1, 4));
    // Oversized and empty requests are rejected.
    ring.Retire(4);
    assert(!ring.Allocate(11, 5));
    assert(!ring.Allocate(0, 5));
    // A wrap charges the skipped tail to the wrapping allocation.
    RingAllocator wrap{8};
    assert(wrap.Allocate(6, 1) == 0u);
    assert(wrap.Allocate(2, 1) == 6u);
    wrap.Retire(1);
    assert(wrap.Allocate(5, 2) == 0u);   // head at 5
    assert(wrap.Allocate(2, 2) == 5u);   // head at 7
    assert(wrap.Allocate(2, 3) == std::nullopt);   // 1 left at the end, none at the front
    assert(wrap.Allocate(1, 3) == 7u);   // head wraps to 8
    assert(wrap.Used() == 8);
    wrap.Retire(2);
    assert(wrap.Used() == 1);
    assert(wrap.Allocate(4, 4) == 0u);   // wraps (pad 0) to the freed front
}
"""


class DescriptorRingTests(unittest.TestCase):
    def test_ring_math(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-ring-") as temp:
            source = Path(temp) / "ring.cpp"
            binary = Path(temp) / "ring"
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


if __name__ == "__main__":
    unittest.main()
