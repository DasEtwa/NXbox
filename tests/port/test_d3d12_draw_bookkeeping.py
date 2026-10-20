# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production bookkeeping methods without the Windows graphics SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import MOCK, function

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


def compile_run(source):
    with tempfile.TemporaryDirectory(prefix="nxbox-bookkeeping-") as temp:
        path = Path(temp) / "test.cpp"
        binary = Path(temp) / "test"
        path.write_text(source)
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
                str(path),
                "-o",
                str(binary),
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stderr)
        subprocess.run([str(binary)], check=True, timeout=30)


class DrawBookkeepingTests(unittest.TestCase):
    def test_generation_retention_and_actual_complete_through(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(source, f"void Context::{name}(") for name in ("Retain", "CompleteThrough")
        )
        compile_run(
            r"""
#include "common/nxbox_wait_diag.h"
#include <cassert>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
using u64 = std::uint64_t;
struct Resource { u64 retained_generation{}; };
using ResourcePtr = std::shared_ptr<Resource>;
namespace EdenXbox { struct UnknownRendererException { const char* message; }; }
struct Context {
    bool recording{};
    bool deferred{};
    u64 acknowledged{};
    u64 LastSubmittedTick() const { return acknowledged; }
    u64 retention_generation{};
    struct Batch {
        u64 tick;
        std::vector<std::function<void()>> callbacks;
        std::vector<ResourcePtr> resources;
    };
    std::deque<Batch> batches;
    std::recursive_mutex completion_mutex;
    std::vector<ResourcePtr> retained;
    std::function<void()> on_commands;
    void Commands() {
        ++retention_generation;
        recording = true;
        if (on_commands) { auto callback = std::move(on_commands); callback(); }
    }
    template <class T> void ReportFailure(const T&) { assert(false); }
    void Retain(const ResourcePtr&);
    void CompleteThrough(u64);
    void Submit(u64 tick, std::function<void()> callback) {
        batches.push_back({tick, {std::move(callback)}, std::move(retained)});
        retained.clear();
        recording = false;
    }
};
"""
            + methods
            + r"""
int main() {
    Context context;
    auto resource = std::make_shared<Resource>();
    std::weak_ptr<Resource> lifetime = resource;
    // Commands can reenter retention before the outer Retain consults the stamp.
    context.on_commands = [&] { context.Retain(resource); };
    context.Retain(resource);
    context.Retain(resource);
    auto other = std::make_shared<Resource>();
    context.Retain(other);
    context.Retain(resource); // Not merely a repeated-last-resource shortcut.
    assert(context.retained.size() == 2 && resource.use_count() == 2);
    int callbacks = 0;
    context.Submit(10, [&] { assert(!lifetime.expired()); ++callbacks; });
    context.Retain(resource);
    assert(context.retained.size() == 1 && resource.use_count() == 3);
    context.Submit(20, [&] { assert(!lifetime.expired()); ++callbacks; });
    resource.reset();
    context.deferred = true;
    context.acknowledged = 9;
    context.CompleteThrough(20);
    assert(callbacks == 0 && !lifetime.expired());
    context.acknowledged = 10;
    context.CompleteThrough(20);
    assert(callbacks == 1 && !lifetime.expired());
    context.acknowledged = 20;
    context.CompleteThrough(20);
    assert(callbacks == 2 && lifetime.expired());
    assert(other.use_count() == 1);
}
"""
        )

    def test_no_barrier_returns_before_profile_scope(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        for name, guard in (
            ("Transition", "resource->heap != D3D12_HEAP_TYPE_DEFAULT || resource->state == state"),
            ("TransitionBatch", "if (!needs_barrier)"),
            ("TransitionSubresources", "if (std::all_of"),
        ):
            method = function(source, f"void Context::{name}(")
            early_return = method.index("return;", method.index(guard))
            self.assertLess(early_return, method.index("FrameProfile::Scope"))
        header = (NATIVE / "d3d12_context.h").read_text()
        self.assertNotIn("retained_ids", header + source)
        self.assertIn("resource->retained_generation == retention_generation", source)
        self.assertIn("next_retention_generation.fetch_add", source)
        # InvalidateRecordedState bumps recording_generation inside one recording; a shared
        # counter could repeat a stamp across recordings and drop a resource still in use.
        invalidate = function(header, "void InvalidateRecordedState()")
        self.assertNotIn("retention_generation", invalidate)

    def test_noop_transitions_keep_versions_without_profile_scopes(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(source, f"void Context::{name}(")
            for name in ("Transition", "TransitionBatch", "TransitionSubresources")
        )
        mock = MOCK.replace(
            "struct Scope { Scope(FrameProfile&, FramePhase) {} };",
            "unsigned scopes{}; struct Scope { Scope(FrameProfile& p, FramePhase) { ++p.scopes; } };",
        )
        compile_run(
            mock
            + methods
            + r"""
int main() {
    Context context;
    auto resource = std::make_shared<Resource>();
    resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    context.Transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::pair<ResourcePtr, unsigned> uses[] = {
        {resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}
    };
    context.TransitionBatch(uses);
    const u32 indices[] = {0, 3, 0};
    context.TransitionSubresources(resource, indices, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.Transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(resource->sub_states.empty());
    assert(resource->content_version == 4);
    assert(context.profile.scopes == 0 && context.recording.calls.empty());
    // Repeated resources with different requested states must keep intermediate barriers.
    const std::pair<ResourcePtr, unsigned> ordered[] = {
        {resource, SrvState}, {resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}
    };
    context.TransitionBatch(ordered);
    assert(context.profile.scopes == 1 && context.recording.calls.size() == 1);
    const auto& barriers = context.recording.calls.back();
    assert(barriers.size() == 2);
    assert(barriers[0].Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(barriers[0].Transition.StateAfter == SrvState);
    assert(barriers[1].Transition.StateBefore == SrvState);
    assert(barriers[1].Transition.StateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
"""
        )

    def test_resource_description_is_queried_once_per_resource(self):
        header = (NATIVE / "d3d12_context.h").read_text()
        accessor = function(header, "const D3D12_RESOURCE_DESC& Description() const")
        compile_run(
            r"""
#include <cassert>
#include <optional>
struct D3D12_RESOURCE_DESC { unsigned width; };
struct Handle {
    mutable unsigned calls{};
    unsigned width = 42;
    D3D12_RESOURCE_DESC GetDesc() const { ++calls; return {width}; }
    const Handle* operator->() const { return this; }
};
struct Resource {
    Handle handle;
    mutable std::optional<D3D12_RESOURCE_DESC> description;
"""
            + accessor
            + r"""
};
int main() {
    Resource resource;
    for (unsigned draw = 0; draw < 1000; ++draw)
        assert(resource.Description().width == 42);
    assert(resource.handle.calls == 1);
    Resource replacement;
    replacement.handle.width = 64;
    assert(replacement.Description().width == 64 && replacement.handle.calls == 1);
    assert(resource.Description().width == 42 && resource.handle.calls == 1);
}
"""
        )

    def test_attachment_snapshot_survives_relocation_and_eviction_without_copies(self):
        header = (NATIVE / "d3d12_texture_cache.h").read_text()
        framebuffer = function(header, "class Framebuffer {") + ";"
        compile_run(
            r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
using u8 = std::uint8_t;
namespace VideoCommon {
constexpr unsigned NUM_RT = 8;
struct Extent2D { unsigned width, height; };
struct RenderTargets { std::array<u8, NUM_RT> draw_buffers{}; Extent2D size{32, 32}; };
}
struct TextureCacheRuntime {};
struct ImageView {
    static inline unsigned copies{};
    std::shared_ptr<int> resource;
    ImageView() = default;
    ImageView(const ImageView& other) : resource{other.resource} { ++copies; }
    ImageView& operator=(const ImageView& other) { resource = other.resource; ++copies; return *this; }
};
"""
            + framebuffer
            + r"""
int main() {
    TextureCacheRuntime runtime;
    ImageView view;
    view.resource = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = view.resource;
    std::array<ImageView*, VideoCommon::NUM_RT> colors{};
    colors[0] = &view;
    std::vector<Framebuffer> cache;
    cache.emplace_back(runtime, colors, &view, VideoCommon::RenderTargets{});
    auto snapshot = cache.front().Snapshot();
    auto* color = snapshot->Color(0);
    const unsigned copies = ImageView::copies;
    for (unsigned i = 0; i < 100; ++i) {
        auto next = cache.front().Snapshot();
        assert(next == snapshot && next->Color(0) == color);
        assert(next->Depth()->resource == color->resource);
    }
    assert(ImageView::copies == copies);
    cache.reserve(512);
    assert(cache.front().Snapshot() == snapshot);
    cache.clear();
    view.resource.reset();
    assert(!lifetime.expired() && *color->resource == 42);
    snapshot.reset();
    assert(lifetime.expired());
}
"""
        )
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        setup = rasterizer.split("const auto framebuffer_snapshot", 1)[1].split(
            "draw_phase.emplace(context.Profile(), FramePhase::ResourceBindings)", 1
        )[0]
        self.assertNotIn("GetDesc()", setup)
        self.assertNotIn("std::vector<ColorCopy>", setup)
        self.assertIn("ResourceDescription()", setup)
        self.assertIn("resource->Description();", (NATIVE / "d3d12_texture_cache.cpp").read_text())
