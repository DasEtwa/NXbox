# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify exclusive timing boundaries and the production macro variant dispatcher."""

import re
import unittest

import test_gpu_frontend_hot_paths as frontend

ROOT = frontend.ROOT
method = frontend.method


class MaxwellMethodBatchingTests(unittest.TestCase):
    compile_and_run = frontend.GpuFrontendHotPathTests.compile_and_run

    def test_nested_phase_skips_clocks_and_restores_draw_and_macro(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        production = "\n".join(
            method(source, f"FrameProfile::{name}").replace("FrameProfile::", "TestProfile::")
            for name in ("Enter", "Leave")
        )
        self.compile_and_run(
            r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_context.h"
using namespace D3D12;
using namespace Common::GpuProfile;
struct TestProfile : Sink {
    FrameTiming timing;
    Tick now{};
    unsigned clocks{};
    void Switch(FramePhase phase) { ++clocks; timing.Switch(phase, now); }
    std::size_t Enter(Phase) override;
    void Leave(std::size_t) override;
};
// PRODUCTION
int main() {
    TestProfile p;
    auto dma = p.Enter(Phase::DmaPusher);
    p.now = 10;
    auto methods = p.Enter(Phase::Maxwell3DMethods);
    for (int i = 0; i < 10000; ++i) {
        auto nested = p.Enter(Phase::Maxwell3DMethods);
        p.Leave(nested);
    }
    assert(p.clocks == 2);
    p.now = 20;
    auto macro = p.Enter(Phase::MacroExecution);
    p.now = 30;
    auto send = p.Enter(Phase::Maxwell3DMethods);
    p.now = 40;
    // Renderer scopes switch the same timing object directly.
    p.Switch(FramePhase::DrawRecording);
    p.now = 50;
    auto cache = p.Enter(Phase::BufferCacheBind);
    p.now = 60;
    p.Leave(cache);
    assert(p.timing.active == FramePhase::DrawRecording);
    p.now = 70;
    p.Switch(FramePhase::Maxwell3DMethods);
    p.now = 80;
    p.Leave(send);
    p.now = 90;
    p.Leave(macro);
    p.now = 100;
    p.Leave(methods);
    p.now = 110;
    p.Leave(dma);
    assert(p.timing.active == FramePhase::BetweenDraws);
    auto elapsed = [&](FramePhase phase) { return p.timing.cpu[static_cast<size_t>(phase)]; };
    assert(elapsed(FramePhase::DmaPusher) == 20);
    assert(elapsed(FramePhase::Maxwell3DMethods) == 40);
    assert(elapsed(FramePhase::MacroExecution) == 20);
    assert(elapsed(FramePhase::DrawRecording) == 20);
    assert(elapsed(FramePhase::BufferCacheBind) == 10);
}
""".replace("// PRODUCTION", production)
        )

    def test_macro_dispatch_all_alternatives_refresh_arguments_and_exceptions(self):
        source = (ROOT / "src/video_core/macro.cpp").read_text()
        start = source.index("    const auto execute_variant =")
        end = source.index("    if (auto const it = macro_cache.find(method);", start)
        dispatch = source[start:end]
        header = (ROOT / "src/video_core/macro.h").read_text()
        variant = re.search(r"using AnyCachedMacro = std::variant<.*?>;", header, re.S).group()
        names = re.findall(r"^    (HLE_\w+|MacroInterpreterImpl),", variant, re.M)
        types = "\n".join(f"struct {name} : Program<{i}> {{}};" for i, name in enumerate(names))
        self.compile_and_run(
            r"""
#include <cassert>
#include <memory>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>
using u32 = unsigned;
#define UNREACHABLE() throw 91
namespace Settings { struct { bool disable_macro_hle{}; } values; }
struct Maxwell { unsigned refreshes{}; void RefreshParameters() { ++refreshes; } };
static unsigned calls, seen_method, seen_parameter, seen_refreshes;
static bool fail;
template <int ID> struct Program {
    void Execute(int&, Maxwell& m, std::span<const u32> params, u32 method) {
        ++calls;
        seen_method = method;
        seen_parameter = params[0];
        seen_refreshes = m.refreshes;
        if (fail) throw ID;
    }
};
struct HLEMacro {};
// TYPES
struct DynamicCachedMacro : Program<100> {};
// VARIANT
void run(AnyCachedMacro& cached) {
    int system{};
    Maxwell maxwell3d;
    u32 method = 543;
    const u32 words[]{123};
    std::span<const u32> parameters{words};
    // DISPATCH
    const bool refresh = Settings::values.disable_macro_hle ||
        std::holds_alternative<MacroInterpreterImpl>(cached) ||
        std::holds_alternative<std::unique_ptr<DynamicCachedMacro>>(cached);
    const auto before = calls;
    execute_variant(cached);
    assert(calls == before + 1 && seen_method == method && seen_parameter == 123);
    assert(seen_refreshes == unsigned(refresh));
    fail = true;
    try { execute_variant(cached); assert(false); } catch (int) {}
    fail = false;
}
template <size_t I = 2> void check() {
    using T = std::variant_alternative_t<I, AnyCachedMacro>;
    AnyCachedMacro cached;
    if constexpr (std::is_same_v<T, std::unique_ptr<DynamicCachedMacro>>)
        cached = std::make_unique<DynamicCachedMacro>();
    else
        cached = T{};
    for (bool disable : {false, true}) { Settings::values.disable_macro_hle = disable; run(cached); }
    if constexpr (I + 1 < std::variant_size_v<AnyCachedMacro>) check<I + 1>();
}
int main() { check(); }
""".replace("// TYPES", types)
            .replace("// VARIANT", variant)
            .replace("// DISPATCH", dispatch)
        )
