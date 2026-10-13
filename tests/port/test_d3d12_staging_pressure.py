# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production staging allocator against a std-only simulated queue."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 Align(u64 size, u64 alignment) { return (size + alignment - 1) & ~(alignment - 1); }
struct Resource {
    std::vector<u8> bytes;
    std::span<u8> mapped;
    explicit Resource(u64 size) : bytes(size), mapped(bytes) {}
};
using ResourcePtr = std::shared_ptr<Resource>;
struct StagingBufferRef { ResourcePtr buffer; size_t offset; std::span<u8> mapped_span; };
enum class FrameCount { ForcedSubmitStagingPressure };
struct Context {
    u64 tick = 1, completed = 0;
    int finishes = 0;
    std::vector<ResourcePtr> retained;
    bool IsFree(u64 value) const { return value <= completed; }
    u64 CurrentTick() const { return tick; }
    void Collect() {}
    void Finish(FrameCount = FrameCount::ForcedSubmitStagingPressure) { ++finishes; completed = tick++; retained.clear(); }
    ResourcePtr CreateBuffer(u64 size, int) { return std::make_shared<Resource>(size); }
};
struct StagingRing {
    struct Page { ResourcePtr resource; u64 cursor; u64 tick; };
    Context& context;
    int heap = 0;
    std::vector<Page> pages;
    void Collect();
    StagingBufferRef Request(size_t size);
};
__IMPLEMENTATION__
int main() {
    constexpr u64 mib = 1ULL << 20;
    Context context;
    StagingRing ring{context};
    std::vector<std::weak_ptr<Resource>> old;
    for (int i = 0; i < 16; ++i) {
        auto lease = ring.Request(4 * mib);
        assert(lease.offset == 0);
        old.push_back(lease.buffer);
        context.retained.push_back(lease.buffer);
    }
    assert(context.finishes == 0 && ring.pages.size() == 16);
    auto next = ring.Request(4 * mib);
    assert(context.finishes == 1 && ring.pages.size() == 16);
    assert(next.offset == 0 && next.buffer == old[0].lock());
    // A larger size class evicts completed smaller pages, preserving a caller's lease.
    next.mapped_span[0] = 42;
    auto large = ring.Request(8 * mib);
    assert(context.finishes == 2 && large.mapped_span.size() == 8 * mib);
    assert(next.mapped_span[0] == 42 && large.buffer != next.buffer);
    assert(ring.pages.size() == 2);
    // A completed page cannot reset its cursor until all CPU leases are released.
    context.Finish();
    ring.Collect();
    assert(ring.pages[0].cursor == 4 * mib);
    next = {};
    ring.Collect();
    assert(ring.pages[0].cursor == 0);
    // Images larger than the ring get a temporary lease without permanently growing it.
    const auto pages = ring.pages.size();
    auto oversized = ring.Request(65 * mib);
    assert(oversized.mapped_span.size() == 65 * mib && ring.pages.size() == pages);
}
"""


class D3D12StagingPressureTests(unittest.TestCase):
    def test_pressure_submits_before_reusing_completed_pages(self):
        context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        implementation = context.split("void StagingRing::Collect()", 1)[1].split(
            "} // namespace D3D12", 1
        )[0]
        source_text = DRIVER.replace(
            "__IMPLEMENTATION__", "void StagingRing::Collect()" + implementation
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-staging-pressure-") as folder:
            source = Path(folder) / "staging.cpp"
            binary = Path(folder) / "staging"
            source.write_text(source_text)
            result = subprocess.run(
                ["clang++", "-std=c++20", str(source), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
