# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check DMA state-run lookup boundaries and inline dispatch elimination."""

import os
import re
import unittest

import test_gpu_frontend_hot_paths as frontend

FIXTURES, MAXWELL, ROOT, method = (
    frontend.FIXTURES,
    frontend.MAXWELL,
    frontend.ROOT,
    frontend.method,
)


class DmaStateRunTests(unittest.TestCase):
    compile_and_run = frontend.GpuFrontendHotPathTests.compile_and_run

    def test_precomputed_runs_match_scalar_mask(self):
        source = MAXWELL.read_text()
        header = MAXWELL.with_suffix(".h").read_text()
        constructor = source[source.index("Maxwell3D::Maxwell3D(") : source.index("Maxwell3D::~")]
        self.assertIn("InitializeRegisterWriteRuns();", constructor)
        getter = re.search(
            r"    u32 GetRegisterWriteRunLength\(u32 method\) const \{.*?\n    \}",
            header,
            re.DOTALL,
        ).group()
        self.compile_and_run(
            r"""
#include <array>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <random>
using u16 = std::uint16_t;
using u32 = std::uint32_t;
struct Maxwell3D {
    std::array<u16, 0xE00> register_write_runs{};
    std::bitset<65535> execution_mask;
    void InitializeRegisterWriteRuns();
// GETTER
};
// INITIALIZER
int main() {
    std::mt19937 rng{91427};
    Maxwell3D engine;
    static_assert(sizeof(engine.register_write_runs) == 7168);
    for (unsigned sample = 0; sample < 100; ++sample) {
        for (unsigned i = 0; i < 0xE00; ++i)
            engine.execution_mask[i] = sample == 0 ? false : sample == 1 ? true : rng() % 17 == 0;
        engine.InitializeRegisterWriteRuns();
        for (u32 i = 0; i < 0xE00; ++i) {
            u32 expected = 0;
            while (i + expected < 0xE00 && !engine.execution_mask[i + expected])
                ++expected;
            assert(engine.GetRegisterWriteRunLength(i) == expected);
        }
    }
}
""".replace("// GETTER", getter).replace(
                "// INITIALIZER", method(source, "Maxwell3D::InitializeRegisterWriteRuns")
            )
        )

    def test_dma_matches_scalar_and_inline_skips_dispatch(self):
        source = (ROOT / "src/video_core/dma_pusher.cpp").read_text()
        parser = method(source, "DmaPusher::ProcessCommands")
        start = parser.index("            constexpr u32 cb_begin")
        end = parser.index("            auto const command_header", start)
        reference = (parser[:start] + parser[end:]).replace(
            "DmaPusher::ProcessCommands", "ReferencePusher::ProcessCommands"
        )
        dispatch = method(source, "DmaPusher::CallMethod").replace(
            "{\n", "{\n    ++scalar_dispatches;\n", 1
        )
        production = "\n".join(
            [dispatch, method(source, "DmaPusher::CallMultiMethod"), parser, reference]
        )
        fixture = (FIXTURES / "gpu_frontend_dma.cpp").read_text()
        getter = (
            re.search(
                r"    u32 GetRegisterWriteRunLength\(u32 method\) const \{.*?\n    \}",
                MAXWELL.with_suffix(".h").read_text(),
                re.DOTALL,
            )
            .group()
            .replace(
                "return register_write_runs", "++run_lookups;\n        return register_write_runs"
            )
        )
        fixture = re.sub(
            r"    u32 GetRegisterWriteRunLength\(u32 method\) const \{.*?\n    \}",
            lambda _: getter,
            fixture,
            flags=re.DOTALL,
        )
        fixture = fixture.replace(
            "    unsigned batches{}, increasing_batches{};",
            "    unsigned batches{}, increasing_batches{};\n"
            "    mutable unsigned run_lookups{};\n"
            "    std::array<std::uint16_t, 0xE00> register_write_runs{};\n"
            "    void InitializeRegisterWriteRuns();",
        ).replace("using u32 =", "using u16 = std::uint16_t;\nusing u32 =")
        fixture = fixture.replace(
            "        std::vector<CommandHeader> stream;",
            "        batch.engine.InitializeRegisterWriteRuns();\n"
            "        batch.engine1.InitializeRegisterWriteRuns();\n"
            "        std::vector<CommandHeader> stream;",
        )
        production += "\n" + method(
            MAXWELL.read_text(), "Maxwell3D::InitializeRegisterWriteRuns"
        ).replace("Maxwell3D::", "Engines::Maxwell3D::")
        # Count calls at the actual production dispatch entry, including the opt-out.
        checks = r"""
    const bool enabled = std::getenv("NXBOX_DMA_STATE_RUNS")[0] == '1';
    DmaPusher increasing;
    increasing.engine.InitializeRegisterWriteRuns();
    CommandHeader run{};
    run.mode = SubmissionMode::Increasing;
    run.method = 0x80;
    run.method_count = 3;
    std::array<CommandHeader, 4> packet{run, {}, {}, {}};
    increasing.ProcessCommands(packet);
    assert(increasing.engine.run_lookups == (enabled ? 1u : 0u));
    assert(increasing.engine.increasing_batches == 1);
    DmaPusher pusher;
    pusher.engine.InitializeRegisterWriteRuns();
    // Every inline write must remain queued until the same sink boundary.
    for (u32 method : {0x40u, 0x80u, 0xDFFu}) {
        CommandHeader header{};
        header.mode = SubmissionMode::Inline;
        header.method = method;
        header.arg_count = 73;
        pusher.ProcessCommands(std::span{&header, 1});
    }
    assert(pusher.scalar_dispatches == (enabled ? 0u : 3u));
    assert(pusher.engine.events.empty());
    assert(pusher.engine.method_sink.size() == 3);
    for (auto [method, value] : pusher.engine.method_sink) {
        (void)method;
        assert(value == 73);
    }
    // Side effects, puller methods, macros and other engines still dispatch.
    pusher.engine.execution_mask[0x81] = true;
    for (u32 method : {0x20u, 0x81u, 0xE00u}) {
        CommandHeader header{};
        header.mode = SubmissionMode::Inline;
        header.method = method;
        pusher.ProcessCommands(std::span{&header, 1});
    }
    CommandHeader other{};
    other.mode = SubmissionMode::Inline;
    other.method = 0x80;
    other.subchannel = 2;
    pusher.ProcessCommands(std::span{&other, 1});
    assert(pusher.scalar_dispatches == (enabled ? 4u : 7u));
"""
        fixture = fixture[: fixture.rfind("}")] + checks + "}\n"
        self.compile_and_run(
            fixture.replace("// METHODS", production),
            [
                dict(
                    os.environ,
                    NXBOX_DMA_STATE_RUNS=value,
                    NXBOX_GPU_FRONTEND_FASTPATH="1",
                    NXBOX_GPU_METHOD_PROFILE="0",
                )
                for value in ("0", "1")
            ],
        )


if __name__ == "__main__":
    unittest.main()
