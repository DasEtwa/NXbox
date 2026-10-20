# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare production binding reuse against uncached resolution with seeded random state."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_t7_buffers import BACKEND, BINDINGS_DRIVER, ROOT


def definition(source, name):
    start = source.index(name)
    return source[start : source.index("\n}", start) + 2]


class BufferBindingDirtyTests(unittest.TestCase):
    def compile_and_run(self, driver):
        with tempfile.TemporaryDirectory(prefix="nxbox-dirty-bindings-") as folder:
            source = Path(folder) / "bindings.cpp"
            binary = Path(folder) / "bindings"
            source.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_random_uniform_and_storage_views_match_old_path(self):
        source = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        # These are the actual production methods, with SDK-independent resources holding bytes.
        implementation = "\n".join(
            definition(source, name)
            for name in (
                "void BufferCacheRuntime::BindVertexBuffer(",
                "std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(",
                "bool BufferCacheRuntime::TryReuseUniformBinding(",
                "void BufferCacheRuntime::BindUniformBuffer(",
                "BoundBuffer BufferCacheRuntime::CopyUniform(",
                "bool BufferCacheRuntime::TryReuseStorageBinding(",
                "void BufferCacheRuntime::BindStorageBuffer(",
            )
        )
        header = (BACKEND / "d3d12_buffer_cache.h").read_text()
        fields = (
            "struct UniformBindingResolution {"
            + header.split("struct UniformBindingResolution {", 1)[1].split(
                "    UniformCopyCache<Resource> uniform_copies;", 1
            )[0]
        )
        fields = fields.replace("VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS", "18")
        fields = fields.replace("VideoCommon::NUM_STORAGE_BUFFERS", "16")
        driver = BINDINGS_DRIVER[: BINDINGS_DRIVER.index("int main() {")]
        driver = driver.replace("__BINDINGS__", implementation).replace(
            "__RESOLUTION_FIELDS__", fields
        )
        driver = driver.replace(
            "if (!page) page = context.CreateBuffer(4 * 1024 * 1024, 0);",
            "if (!page || cursor + size + 511 > page->bytes.size()) { "
            "page = context.CreateBuffer(4 * 1024 * 1024, 0); cursor = 0; }",
        )
        driver = (
            "#include <random>\n"
            + driver
            + r"""
void Same(const BoundBuffer& a, const BoundBuffer& b) {
    assert(bool(a.resource) == bool(b.resource));
    assert(a.size == b.size && a.written == b.written);
    if (a.resource) {
        assert(a.offset + a.size <= a.resource->bytes.size());
        assert(b.offset + b.size <= b.resource->bytes.size());
        assert(std::equal(a.resource->bytes.begin() + a.offset,
                          a.resource->bytes.begin() + a.offset + a.size,
                          b.resource->bytes.begin() + b.offset));
    }
}
int main() {
    BufferCacheRuntime old, fast;
    old.binding_cache = fast.binding_cache = false; // No experimental snapshot reuse.
    fast.resolution_cache = true;
    std::mt19937 random(0xB17D1);
    std::array<Buffer, 3> buffers{Buffer{old.context.CreateBuffer(131072, 0)},
                                Buffer{old.context.CreateBuffer(131072, 0)}, Buffer{nullptr}};
    unsigned uniform_hits = 0, storage_hits = 0;
    for (unsigned draw = 0; draw < 1500; ++draw) {
        const unsigned stage = random() % 5;
        old.bindings.current_stage = fast.bindings.current_stage = stage;
        old.bindings.stages[stage].uniform.clear();
        fast.bindings.stages[stage].uniform.clear();
        old.bindings.stages[stage].storage.clear();
        fast.bindings.stages[stage].storage.clear();
        if (draw % 17 == 0) { // Content changes without an address/size change.
            auto resource = buffers[0].Handle();
            std::fill(resource->bytes.begin(), resource->bytes.end(), u8(random()));
            ++resource->content_version;
        }
        if (draw % 73 == 0) // Simulate deletion/merge/ID recycling with a new resource.
            buffers[1] = Buffer{old.context.CreateBuffer(131072, 0)};
        if (draw % 41 == 0) fast.InvalidateUniformBindings();
        for (unsigned slot = 0, count = 1 + random() % 23; slot < count; ++slot) {
            const Buffer& buffer = buffers[(draw % 3 == 0) ? random() % 3 : 0];
            const u32 offset = draw % 3 ? 256 : random() % 131080;
            const u32 size = draw % 3 ? 256 : random() % 66000;
            const u32 limit = (random() & 1) ? size : ~0U;
            const bool written = random() & 1;
            old.SetUniformReadLimit(limit);
            old.BindUniformBuffer(buffer, offset, size);
            if (fast.TryReuseUniformBinding(buffer, offset, size, limit)) ++uniform_hits;
            else {
                fast.SetUniformReadLimit(limit);
                fast.BindUniformBuffer(buffer, offset, size);
            }
            Same(old.bindings.stages[stage].uniform.back(),
                 fast.bindings.stages[stage].uniform.back());
            old.BindStorageBuffer(buffer, offset, size, written);
            if (fast.TryReuseStorageBinding(buffer, offset, size, written)) ++storage_hits;
            else fast.BindStorageBuffer(buffer, offset, size, written);
            Same(old.bindings.stages[stage].storage.back(),
                 fast.bindings.stages[stage].storage.back());
        }
    }
    assert(uniform_hits > 100 && storage_hits > 100);
}
"""
        )
        self.compile_and_run(driver)

    def test_random_geometry_keys_match_unconditional_binds(self):
        source = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        methods = "\n".join(
            definition(source, name)
            for name in (
                "bool BufferCacheRuntime::TryReuseIndexBinding(",
                "bool BufferCacheRuntime::TryReuseVertexBinding(",
                "void BufferCacheRuntime::BindIndexBuffer(",
                "void BufferCacheRuntime::BindVertexBuffer(",
            )
        )
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <random>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Maxwell {
enum class PrimitiveTopology { Triangles, Quads, QuadStrip };
enum class IndexFormat { UnsignedByte, UnsignedShort, UnsignedInt };
}
struct Resource {
    struct Desc { u64 Width = 65536; };
    Desc Description() { return {}; }
    Desc GetDesc() { return {}; }
    u64 GetGPUVirtualAddress() { return 0; }
    Resource* handle = this;
};
using ResourcePtr = std::shared_ptr<Resource>;
struct Buffer { ResourcePtr resource; ResourcePtr Handle() const { return resource; } };
struct GeometryBindings {
    static constexpr u32 MaxVertexBuffers = 32;
    struct Vertex { ResourcePtr resource; u64 offset{}, size{}; u32 stride{};
                    bool operator==(const Vertex&) const = default; };
    std::array<Vertex, 32> vertex{};
    ResourcePtr index;
    u64 index_offset{}, index_size{};
    bool index_is_32bit{}, index_unsupported{};
    bool operator==(const GeometryBindings&) const = default;
};
bool trace = false;
bool UploadTraceEnabled() { return trace; }
void InvalidRange(u32, const char*, u64, u64, u64) {}
namespace fmt { template<class... T> int format(const char*, T...) { return 0; } }
void Diagnostic(int) {}
struct BufferCacheRuntime {
    struct { GeometryBindings geometry; } bindings;
    struct { u64 CurrentTick() { return 0; } } context;
    bool enabled = false;
    bool BindingResolutionCacheEnabled() { return enabled; }
    bool BindingCacheEnabled() { return false; }
    bool BufferFastpathEnabled() { return true; }
    bool TryReuseIndexBinding(Maxwell::PrimitiveTopology, Maxwell::IndexFormat,
                              const Buffer&, u32, u32);
    bool TryReuseVertexBinding(u32, const Buffer&, u32, u32, u32);
    void BindIndexBuffer(Maxwell::PrimitiveTopology, Maxwell::IndexFormat,
                         u32, u32, ResourcePtr, u32, u32);
    void BindVertexBuffer(u32, ResourcePtr, u32, u32, u32);
};
__METHODS__
int main() {
    BufferCacheRuntime old, fast;
    fast.enabled = true;
    std::mt19937 random(0x6E01);
    std::array<Buffer, 3> buffers{Buffer{std::make_shared<Resource>()},
                                Buffer{std::make_shared<Resource>()}, Buffer{nullptr}};
    unsigned hits = 0;
    for (unsigned draw = 0; draw < 20000; ++draw) {
        const auto topology = Maxwell::PrimitiveTopology(random() % 3);
        const auto format = Maxwell::IndexFormat(random() % 3);
        const Buffer& buffer = buffers[random() % 3];
        const u32 offset = random() % 4 ? 256 : random() % 70000;
        const u32 size = random() % 4 ? 256 : random() % 70000;
        trace = random() % 7 == 0;
        old.BindIndexBuffer(topology, format, 0, 0, buffer.Handle(), offset, size);
        if (fast.TryReuseIndexBinding(topology, format, buffer, offset, size)) ++hits;
        else fast.BindIndexBuffer(topology, format, 0, 0, buffer.Handle(), offset, size);
        const u32 slot = random() % 34, stride = random() % 3;
        old.BindVertexBuffer(slot, buffer.Handle(), offset, size, stride);
        if (fast.TryReuseVertexBinding(slot, buffer, offset, size, stride)) ++hits;
        else fast.BindVertexBuffer(slot, buffer.Handle(), offset, size, stride);
        assert(old.bindings.geometry == fast.bindings.geometry);
        if (draw % 79 == 0) { // Quad emulation overwrites the live index binding.
            old.bindings.geometry.index_size = fast.bindings.geometry.index_size = 0;
            old.bindings.geometry.index_is_32bit = fast.bindings.geometry.index_is_32bit = true;
        }
    }
    assert(hits > 100);
}
"""
        self.compile_and_run(driver.replace("__METHODS__", methods))

    def test_random_slot_resolution_invalidation_and_fences_match_old_path(self):
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        helper = definition(source, "BufferId BufferCache<P>::ResolveBufferBinding(")
        slot_source = (ROOT / "src/common/slot_vector.h").read_text()
        slot = slot_source.split("struct SlotId {", 1)[1].split("\n};", 1)[0]
        driver = r"""
#include <array>
#include <cassert>
#include <compare>
#include <cstdint>
#include <limits>
#include <random>
using u32 = std::uint32_t;
using DAddr = std::uint64_t;
struct SlotId { __SLOT__
};
using BufferId = SlotId;
struct Binding { DAddr device_addr{}; u32 size{}; BufferId buffer_id; };
namespace Settings {
bool accurate = false;
bool IsGPUFenceBehaviorAccurate() { return accurate; }
}
struct Runtime { bool enabled; bool BindingResolutionCacheEnabled() { return enabled; } };
template<class P> struct BufferCache {
    Runtime runtime;
    struct Slots {
        std::array<unsigned, 8> data{};
        unsigned& operator[](BufferId id) { return data[id.index]; }
    } slot_buffers;
    unsigned finds{}, waits{};
    BufferId FindBuffer(DAddr address, u32, bool) {
        ++finds;
        if (!address) return BufferId{0};
        const auto id = BufferId{u32(address / 4096)};
        if (Settings::accurate) SynchronizeBufferWrites(slot_buffers[id]);
        return id;
    }
    void SynchronizeBufferWrites(unsigned&) { ++waits; }
    BufferId ResolveBufferBinding(const Binding&, DAddr, u32);
};
template<class P>
__HELPER__
int main() {
    BufferCache<int> old{}, fast{};
    fast.runtime.enabled = true;
    std::array<Binding, 32> slots{};
    std::mt19937 random(0xD171);
    for (unsigned i = 0; i < 100000; ++i) {
        Binding& previous = slots[random() % slots.size()];
        DAddr address = previous.device_addr;
        u32 size = previous.size;
        if (random() % 5 == 0) { address = (random() % 8) * 4096; size = random() % 4096; }
        if (random() % 7 == 0) previous.buffer_id = BufferId{}; // Delete/merge invalidation.
        Settings::accurate = random() & 1;
        const auto expected = old.ResolveBufferBinding(previous, address, size);
        const auto actual = fast.ResolveBufferBinding(previous, address, size);
        assert(actual == expected && old.waits == fast.waits);
        previous = {address, size, actual};
    }
    assert(fast.finds < old.finds / 2);
}
"""
        self.compile_and_run(driver.replace("__SLOT__", slot).replace("__HELPER__", helper))


if __name__ == "__main__":
    unittest.main()
