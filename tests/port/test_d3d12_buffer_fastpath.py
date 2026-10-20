# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual binding methods without the Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index("template <class P>\nvoid BufferCache<P>::" + name)
    return source[start : source.index("\n}", start) + 2]


class BufferFastpathTests(unittest.TestCase):
    def test_deletion_clears_kept_uniform_ids_in_every_channel(self):
        # Duplicate notifications keep buffer_id, so DeleteBuffer is what keeps it from going stale.
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        delete = function(source, "DeleteBuffer(")
        self.assertIn("std::ranges::for_each(channel_state->uniform_buffers, replace);", delete)
        self.assertIn("for (const size_t id : active_channel_ids)", delete)
        self.assertIn("std::ranges::for_each(channel_storage[id].uniform_buffers, replace);", delete)

    def test_duplicate_bindings_remaps_deletion_and_legacy_runtimes(self):
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        driver = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
using u32 = std::uint32_t;
using GPUVAddr = std::uint64_t;
using DAddr = std::uint64_t;
using BufferId = u32;
struct Binding { DAddr device_addr{}; u32 size{}; BufferId buffer_id{}; };
struct Memory {
    DAddr mapping = 0x1000;
    std::optional<DAddr> GpuToCpuAddress(GPUVAddr) { return mapping; }
};
struct State {
    std::array<std::array<Binding, 2>, 2> uniform_buffers{};
    std::array<u32, 2> enabled_uniform_buffer_masks{3, 3};
    std::array<u32, 2> dirty_uniform_buffers{};
};
struct FastRuntime {
    bool enabled = true;
    bool BufferFastpathEnabled() const { return enabled; }
};
struct LegacyRuntime {};
template <class Runtime> struct BufferCache {
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = true;
    Memory memory;
    Memory* gpu_memory = &memory;
    State state;
    State* channel_state = &state;
    Runtime runtime;
    unsigned finds{};
    BufferId FindBuffer(DAddr, u32, bool) { ++finds; return 42; }
    template <class F> void ForEachEnabledBit(u32 mask, F f) {
        for (u32 index = 0; mask; ++index, mask >>= 1)
            if (mask & 1) f(index);
    }
    void BindGraphicsUniformBuffer(std::size_t, u32, GPUVAddr, u32);
    void UpdateUniformBuffers(std::size_t);
};
__BIND__
__UPDATE__
template <class R> void CheckLegacy(BufferCache<R>& cache) {
    cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 64);
    cache.UpdateUniformBuffers(0);
    assert(cache.state.uniform_buffers[0][0].buffer_id == 42);
    const auto before = cache.finds;
    cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 64);
    assert(cache.state.uniform_buffers[0][0].buffer_id == 0);
    cache.UpdateUniformBuffers(0);
    assert(cache.finds == before + 1);
}
int main() {
    BufferCache<FastRuntime> cache;
    cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 64);
    cache.UpdateUniformBuffers(0);
    const auto before = cache.finds;
    cache.state.dirty_uniform_buffers[0] = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 64);
        cache.UpdateUniformBuffers(0);
    }
    assert(cache.finds == before); // No repeat lookup/fence check on duplicate notifications.
    assert(cache.state.dirty_uniform_buffers[0] == 0);
    cache.memory.mapping = 0x3000; // Same guest pointer, different device mapping.
    cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 64);
    assert(cache.state.uniform_buffers[0][0].buffer_id == 0);
    assert(cache.state.uniform_buffers[0][0].device_addr == 0x3000);
    cache.UpdateUniformBuffers(0);
    cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 128);
    assert(cache.state.uniform_buffers[0][0].buffer_id == 0);
    cache.UpdateUniformBuffers(0);
    cache.state.uniform_buffers[0][0].buffer_id = 0; // Cache deletion/merge invalidation.
    const auto invalidated = cache.finds;
    cache.BindGraphicsUniformBuffer(0, 0, 0x2000, 128);
    cache.UpdateUniformBuffers(0);
    assert(cache.finds == invalidated + 1);
    cache.BindGraphicsUniformBuffer(1, 0, 0x2000, 128);
    assert(cache.state.uniform_buffers[1][0].buffer_id == 0);
    assert(cache.state.uniform_buffers[0][0].buffer_id == 42);
    cache.runtime.enabled = false;
    CheckLegacy(cache);
    BufferCache<LegacyRuntime> legacy;
    CheckLegacy(legacy); // A runtime without the opt-in hook still compiles and behaves as before.
}
"""
        driver = driver.replace("__BIND__", function(source, "BindGraphicsUniformBuffer("))
        driver = driver.replace("__UPDATE__", function(source, "UpdateUniformBuffers("))
        with tempfile.TemporaryDirectory(prefix="nxbox-buffer-fastpath-") as directory:
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
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
