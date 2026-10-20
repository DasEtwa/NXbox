# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production draw-state merge with indexed and linear resource lookup."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import function

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"

MOCK = r"""
#include "video_core/renderer_d3d12/d3d12_draw_resource_index.h"
#include "video_core/renderer_d3d12/d3d12_feedback.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>
using namespace D3D12;
using u32 = unsigned;
using D3D12_RESOURCE_STATES = unsigned;
constexpr unsigned D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 8;
constexpr unsigned D3D12_RESOURCE_STATE_RENDER_TARGET = 4;
constexpr unsigned D3D12_RESOURCE_STATE_DEPTH_WRITE = 16;
constexpr unsigned D3D12_RESOURCE_STATE_STREAM_OUT = 256;
constexpr unsigned D3D12_RESOURCE_DIMENSION_BUFFER = 1;
struct Desc {
    unsigned Dimension = 1, Width = 4096, Height = 1, DepthOrArraySize = 1;
    unsigned MipLevels = 1, Format = 0, Flags = 0;
};
struct Resource {
    Resource* handle = this;
    Desc desc;
    Desc GetDesc() const { return desc; }
};
using ResourcePtr = std::shared_ptr<Resource>;
namespace fmt {
template<class... T> std::string format(const char*, const T&...) { return {}; }
}
void Diagnostic(const std::string&) {}
bool resource_state_index = true;
struct CountedIdentity {
    const void* identity;
    unsigned* comparisons;
    const void* get() const { ++*comparisons; return identity; }
};
struct Harness {
    DrawResourceIndex<> draw_resource_index;
    std::vector<std::pair<ResourcePtr, unsigned>> state_scratch;
    bool Run(const std::vector<std::pair<ResourcePtr, unsigned>>& uses) {
        state_scratch.clear();
        draw_resource_index.Reset();
        bool invalid_states = false;
        MERGE_LAMBDA;
        for (const auto& [resource, state] : uses)
            need(resource, state);
        return invalid_states;
    }
};
int main(int argc, char**) {
    std::mt19937 random(0x48331);
    std::vector<ResourcePtr> resources;
    for (unsigned i = 0; i < 300; ++i) {
        auto resource = std::make_shared<Resource>();
        resource->desc.Dimension = i % 2 ? 1 : 3;
        resources.push_back(std::move(resource));
    }
    Harness slow, fast;
    std::vector<std::pair<ResourcePtr, unsigned>> uses;
    constexpr unsigned states[] = {1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 1024};
    for (unsigned draw = 0; draw < 1500; ++draw) {
        uses.clear();
        const auto count = draw % 7 == 0 ? 300 : 1 + random() % 60;
        // Include oversized draws, nulls, UAV/read aliases and write conflicts.
        for (unsigned i = 0; i < count; ++i) {
            uses.emplace_back(resources[i], states[random() % std::size(states)]);
            uses.emplace_back(resources[random() % count], states[random() % std::size(states)]);
        }
        uses.emplace_back(nullptr, 8);
        resource_state_index = false;
        const bool slow_invalid = slow.Run(uses);
        resource_state_index = true;
        assert(fast.Run(uses) == slow_invalid);
        assert(fast.state_scratch == slow.state_scratch);
        // Reset after an aborted draw (only a prefix was collected).
        uses.resize(1);
        fast.Run(uses);
    }
    // Force collisions and overflow a two-slot index. Subsequent
    // appends relocate the vector; cached indices must remain valid.
    DrawResourceIndex<2> tiny;
    std::vector<std::pair<ResourcePtr, unsigned>> entries;
    for (unsigned i = 0; i < resources.size(); ++i) {
        assert(tiny.FindOrInsert(resources[i].get(), entries) == entries.size());
        entries.emplace_back(resources[i], 0);
    }
    for (unsigned i = 0; i < resources.size(); ++i)
        assert(tiny.FindOrInsert(resources[i].get(), entries) == i);
    // Count actual identity comparisons, rather than timing a test. These
    // aligned addresses hash to distinct buckets; no fake pointer is dereferenced.
    DrawResourceIndex<> counted;
    std::vector<std::pair<CountedIdentity, unsigned>> counted_entries;
    unsigned comparisons = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < 32; ++i) {
            const auto identity = reinterpret_cast<const void*>(std::uintptr_t{0x1000 + i * 16});
            const auto index = counted.FindOrInsert(identity, counted_entries);
            assert(index == i);
            if (index == counted_entries.size())
                counted_entries.emplace_back(CountedIdentity{identity, &comparisons}, 0);
        }
    }
    // The original scan performs 1,024 comparisons for this same sequence.
    assert(comparisons < 128);
    counted.Reset();
    counted_entries.clear();
    const auto identity = reinterpret_cast<const void*>(std::uintptr_t{0x2000});
    assert(counted.FindOrInsert(identity, counted_entries) == 0);
    counted_entries.emplace_back(CountedIdentity{identity, &comparisons}, 0);
    assert(counted.FindOrInsert(identity, counted_entries) == 0);
    static_assert(sizeof(DrawResourceIndex<>) <= 2048);
    if (argc > 1) {
        // Optional host timing, never a wall-clock pass/fail assertion.
        uses.clear();
        for (unsigned i = 0; i < 32; ++i)
            uses.emplace_back(resources[i], 64);
        for (unsigned i = 0; i < 32; ++i)
            uses.emplace_back(resources[i], 128);
        for (bool enabled : {false, true}) {
            resource_state_index = enabled;
            const auto start = std::chrono::steady_clock::now();
            std::size_t checksum = 0;
            for (unsigned frame = 0; frame < 100; ++frame) {
                for (unsigned draw = 0; draw < 3580; ++draw) {
                    const bool invalid = fast.Run(uses);
                    assert(!invalid);
                    checksum += fast.state_scratch.size() + invalid;
                }
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("indexed=%d ms/frame=%.3f checksum=%zu\n", enabled, elapsed / 100, checksum);
        }
    }
}
"""


class DrawResourceIndexTests(unittest.TestCase):
    def test_production_merge_matches_linear_lookup(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        merge = function(source, "const auto need = [this, &invalid_states]")
        self.assertIn("draw_resource_index.FindOrInsert", merge)
        self.assertIn("DrawResourceIndex<>::FindLinear", merge)
        prefix = source.split("const auto need = [this, &invalid_states]", 1)[0]
        self.assertIn('std::getenv("NXBOX_D3D12_RESOURCE_STATE_INDEX")', prefix)
        self.assertIn("draw_resource_index.Reset();", prefix)
        with tempfile.TemporaryDirectory(prefix="nxbox-resource-index-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(MOCK.replace("MERGE_LAMBDA", merge))
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
