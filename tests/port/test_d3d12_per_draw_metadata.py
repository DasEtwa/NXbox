# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production attachment bookkeeping with counted immutable view snapshots."""

import unittest
from pathlib import Path

from test_d3d12_draw_bookkeeping import compile_run
from test_d3d12_subresource_feedback import function

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class PerDrawMetadataTests(unittest.TestCase):
    def test_snapshot_and_count_invalidation_matches_uncached_draws(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        setup = source.split("    static const bool cache_draw_metadata =", 1)[1]
        setup = setup.split("    Pso::GraphicsPsoState attachment_state{};", 1)[0]
        setup = setup.split("\n", 1)[1]
        area = source.split("    const auto extent = [](const ImageView& view)", 1)[1]
        area = (
            "    const auto extent = [](const ImageView& view)"
            + area.split("    std::array<ResourcePtr, VideoCommon::NUM_RT> color_resources{};", 1)[
                0
            ]
        )
        metadata = function((NATIVE / "d3d12_rasterizer.h").read_text(), "struct DrawAttachments {")
        driver = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "__RENDER_AREA__"
using namespace D3D12;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace VideoCommon { constexpr unsigned NUM_RT = 8; }
constexpr unsigned D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET = 1;
constexpr unsigned D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL = 2;
enum D3D12_RESOURCE_DIMENSION { D3D12_RESOURCE_DIMENSION_UNKNOWN,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
struct Desc {
    u64 Width = 128;
    u32 Height = 64, Flags = 3;
    struct { u32 Count = 1, Quality = 0; } SampleDesc;
    D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
};
struct ImageView {
    static inline unsigned reads{};
    std::shared_ptr<int> resource = std::make_shared<int>(1);
    Desc desc;
    struct { unsigned Format = 42; } description;
    struct { struct { unsigned level{}, layer{}; } base;
             struct { unsigned layers = 1; } extent; } range;
    const auto& BorrowImageHandle() const { return resource; }
    const auto& ResourceDescription() const { ++reads; return desc; }
    const auto& Description() const { return description; }
    void LogUnsupported(const std::string&) const {}
};
struct Framebuffer {
    struct Attachments {
        std::array<std::optional<ImageView>, 8> colors{};
        std::optional<ImageView> depth;
        std::array<u8, 8> mapping{0,1,2,3,4,5,6,7};
        struct { unsigned width = 99, height = 55; } size;
        auto* Color(unsigned i) { return colors[i] ? &*colors[i] : nullptr; }
        auto* Depth() { return depth ? &*depth : nullptr; }
        auto DrawBuffer(unsigned i) const { return mapping[i]; }
        auto Size() const { return size; }
    };
};
namespace Pso {
struct FramebufferFormats {
    std::array<u32, 8> colors{};
    u32 depth{};
    u8 sample_count = 1;
    bool operator==(const FramebufferFormats&) const = default;
};
bool RenderTargetLayersOverlap(u32 a, u32 an, u32 b, u32 bn) {
    return a < b + bn && b < a + an;
}
}
u32 DsvFormat(u32 format) { return format; }
namespace fmt { template<class... T> std::string format(const char*, T...) { return {}; } }
struct Result {
    Pso::FramebufferFormats formats;
    std::array<ImageView*, 8> colors;
    AttachmentExtent area, matching;
    bool operator==(const Result&) const = default;
};
struct Draw {
    __METADATA__ draw_attachments;
    struct { struct { u32 count{}; } rt_control; } regs;
    std::optional<Result> result;
    void SkipDraw(const char*) {}
    void Prepare(std::shared_ptr<Framebuffer::Attachments> framebuffer_snapshot,
                 u32 count, bool cache_draw_metadata = true) {
        result.reset();
        regs.rt_control.count = count;
        auto* framebuffer = framebuffer_snapshot.get();
        __SETUP__
        __AREA__
        result = Result{formats, color_views, render_area, matching_extent};
    }
};
int main() {
    Draw cached, baseline;
    auto frame = std::make_shared<Framebuffer::Attachments>();
    frame->colors[0].emplace();
    const auto check = [&](auto snapshot, unsigned count) {
        baseline.Prepare(snapshot, count, false);
        cached.Prepare(snapshot, count);
        assert(cached.result == baseline.result);
    };
    check(frame, 1);
    const auto reads = ImageView::reads;
    for (unsigned i = 0; i < 1000; ++i) cached.Prepare(frame, 1);
    assert(ImageView::reads == reads); // No repeated view validation/extent work.
    check(frame, 0); // Same snapshot, different guest count must invalidate.
    assert(cached.result->area == (AttachmentExtent{99,55,1}));
    check(frame, 1);
    assert(cached.result->area == (AttachmentExtent{128,64,1}));
    auto replacement = std::make_shared<Framebuffer::Attachments>(*frame);
    replacement->colors[1] = replacement->colors[0];
    replacement->colors[1]->range.base.level = 1;
    check(replacement, 2); // Distinct mips of one resource are legal.
    assert(cached.result->area.width == 64 && cached.result->formats.colors[1] == 42);
    replacement = std::make_shared<Framebuffer::Attachments>(*replacement);
    replacement->colors[1]->range.base.level = 0;
    check(replacement, 2); // Overlapping RTV must be suppressed.
    assert(cached.result->colors[1] == nullptr);
    replacement = std::make_shared<Framebuffer::Attachments>(*replacement);
    replacement->mapping[0] = 1;
    replacement->colors[1]->desc.Width = 32;
    replacement->depth.emplace();
    check(replacement, 1);
    assert(cached.result->area.width == 32 && cached.result->matching.width == 128);
    auto invalid = std::make_shared<Framebuffer::Attachments>(*replacement);
    invalid->depth->desc.SampleDesc.Count = 4;
    check(invalid, 1);
    assert(!cached.result); // Sample mismatch cannot reuse previous valid metadata.
    check(replacement, 1);
    invalid = std::make_shared<Framebuffer::Attachments>(*replacement);
    invalid->depth->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    check(invalid, 1);
    assert(!cached.result);
    check(replacement, 1);
    invalid = std::make_shared<Framebuffer::Attachments>(*replacement);
    invalid->colors[1]->desc.Flags = 0;
    check(invalid, 1);
    assert(!cached.result);
    check(replacement, 1);
    auto empty = std::make_shared<Framebuffer::Attachments>();
    empty->size = {0,0};
    check(empty, 0);
    assert(!cached.result);
    check(replacement, 1);
    std::weak_ptr<Framebuffer::Attachments> lifetime = replacement;
    baseline.draw_attachments.snapshot.reset();
    replacement.reset();
    assert(!lifetime.expired()); // Cache owns its pointers, even after eviction.
    check(frame, 1);
    assert(lifetime.expired());
    // A/B override always recalculates; it does not leave the area cache valid.
    auto before = ImageView::reads;
    cached.Prepare(frame, 1, false);
    cached.Prepare(frame, 1, false);
    assert(ImageView::reads > before);
}
"""
        compile_run(
            driver.replace("__RENDER_AREA__", str(NATIVE / "d3d12_render_area.h"))
            .replace("__METADATA__", metadata)
            .replace("__SETUP__", setup)
            .replace("__AREA__", area)
        )

    def test_idle_queries_preserve_predication_and_wake_for_every_query_source(self):
        source = (NATIVE / "d3d12_query_cache.cpp").read_text()
        begin = function(source, "void QueryCacheRuntime::BeginDraw(")
        # Exercise the real EndDraw early exits; active query emission is tested separately.
        end = function(source, "void QueryCacheRuntime::EndDraw()")
        end = end.split("    auto& context = impl->context;", 1)[0]
        end += "    ++impl->active_ends; impl->drawing = false;\n}"
        cancel = function(source, "void QueryCacheRuntime::CancelDraw()")
        compile_run(
            r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
using u32 = std::uint32_t;
constexpr u32 BankSize = 256;
constexpr unsigned D3D12_QUERY_TYPE_PIPELINE_STATISTICS = 1;
constexpr unsigned D3D12_QUERY_TYPE_OCCLUSION = 2;
u32 SOType(u32 stream) { return 3 + stream; }
bool disabled{};
bool BisectEnabled(const char* name) {
    return std::string_view(name) == "NXBOX_D3D12_NO_QUERIES" && disabled;
}
enum class QueryType { ZPassPixelCount, ZPassPixelCount64 };
struct Heap { int Get() const { return 1; } };
struct Commands { unsigned begins{}; void BeginQuery(int, unsigned, unsigned) { ++begins; } };
struct Context {
    struct Commands commands;
    auto* Commands() { return &commands; }
    void MarkWork(const char*) {}
};
struct Impl {
    struct Draw { bool occlusion{}, geometry{}, pipeline{}, recorded{}; u32 streams{}; };
    std::array<Draw, BankSize> draws{};
    std::array<bool, 2> enabled{};
    struct Feedback { u32 mask{}; u32 StreamMask() const { return mask; } } feedback;
    Context context;
    Heap pipeline, occlusion;
    std::array<Heap, 4> so;
    bool drawing{}, idle_draw{}, pipeline_wanted{};
    u32 count{}, slots{}, active_ends{};
    void RequireClosed() { if (drawing) throw std::logic_error("open"); }
    u32 Slot() { ++slots; return count; }
};
struct QueryCacheRuntime {
    Impl* impl;
    unsigned resumes{}, pauses{};
    void ResumeHostConditionalRendering() { ++resumes; }
    void PauseHostConditionalRendering() { ++pauses; }
    void BeginDraw(bool geometry);
    void EndDraw();
    void CancelDraw();
};
"""
            + begin
            + "\n"
            + end
            + "\n"
            + cancel
            + r"""
int main() {
    Impl impl;
    QueryCacheRuntime runtime{&impl};
    // A stale recorded slot must not be read or overwritten by idle draws.
    impl.draws[0].recorded = true;
    for (unsigned i = 0; i < 1000; ++i) {
        runtime.BeginDraw(false);
        assert(impl.drawing);
        runtime.EndDraw();
    }
    assert(runtime.resumes == 1000 && impl.slots == 0);
    assert(impl.draws[0].recorded && impl.active_ends == 0 && !impl.drawing);
    runtime.BeginDraw(false);
    runtime.CancelDraw();
    assert(!impl.drawing && runtime.pauses == 1 && impl.slots == 0);
    for (unsigned source = 0; source < 4; ++source) {
        impl.enabled = {source == 0, source == 1};
        impl.pipeline_wanted = source == 2;
        impl.feedback.mask = source == 3 ? 8 : 0;
        runtime.BeginDraw(true);
        assert(!impl.idle_draw && impl.draws[0].recorded);
        assert(impl.draws[0].geometry);
        runtime.EndDraw();
    }
    assert(impl.active_ends == 4 && impl.context.commands.begins == 4);
    impl.enabled = {}; impl.pipeline_wanted = false; impl.feedback.mask = 0;
    runtime.BeginDraw(false); runtime.EndDraw();
    assert(impl.idle_draw && impl.active_ends == 4);
    bool rejected{};
    try { runtime.EndDraw(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    runtime.BeginDraw(false);
    rejected = false;
    try { runtime.BeginDraw(false); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    runtime.CancelDraw();
    disabled = true;
    runtime.BeginDraw(false); runtime.EndDraw();
    assert(!impl.drawing);
}
"""
        )

    def test_completion_snapshot_respects_recorder_ack_and_device_loss(self):
        method = function(
            (NATIVE / "d3d12_context.cpp").read_text(), "u64 Context::CompletedTick()"
        )
        compile_run(
            r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <stdexcept>
using u64 = std::uint64_t;
struct Fence {
    u64 completed{};
    u64 GetCompletedValue() const { return completed; }
    const Fence* operator->() const { return this; }
};
struct Device {
    int reason{};
    int GetDeviceRemovedReason() const { return reason; }
    const Device* operator->() const { return this; }
    const Device* Get() const { return this; }
};
void Check(int reason, const char*, const Device*) {
    if (reason) throw std::runtime_error("device removed");
}
struct Context {
    Fence fence;
    Device device;
    bool deferred{};
    u64 acknowledged{};
    u64 LastSubmittedTick() const { return acknowledged; }
    u64 CompletedTick() const;
};
"""
            + method
            + r"""
int main() {
    Context context;
    context.fence.completed = 42;
    assert(context.CompletedTick() == 42);
    context.deferred = true; context.acknowledged = 40;
    assert(context.CompletedTick() == 40);
    context.acknowledged = 44;
    assert(context.CompletedTick() == 42);
    context.fence.completed = UINT64_MAX;
    for (int reason : {0, -1}) {
        context.device.reason = reason;
        bool rejected{};
        try { (void)context.CompletedTick(); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
}
"""
        )


if __name__ == "__main__":
    unittest.main()
