# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute Xbox JIT policy and telemetry with actual Dynarmic configurations."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class T10JitTests(unittest.TestCase):
    def run_cpp(self, program, environment=None, runs=((),)):
        with tempfile.TemporaryDirectory(prefix="nxbox-t10-") as directory:
            source = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            source.write_text(program)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    f"-I{ROOT / 'src'}",
                    f"-I{ROOT / 'src/dynarmic/src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            env = {k: v for k, v in os.environ.items() if not k.startswith("NXBOX_")}
            env.update(environment or {})
            for args in runs:
                subprocess.run(
                    [str(binary), str(directory), *args],
                    check=True,
                    timeout=30,
                    env=env,
                )

    def test_default_policy_preserves_desktop_and_accuracy_modes(self):
        self.run_cpp(
            r"""
#include <cassert>
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/config.h"
#include "core/arm/dynarmic/nxbox_jit_policy.h"
template<class C> void Check() {
    C config{};
    const auto original = config.optimizations;
    Core::NxboxJit::ApplyOptOuts(config);
    assert(config.optimizations == original);
    assert(!config.fastmem_pointer);
    config.optimizations = Dynarmic::no_optimizations;
    config.unsafe_optimizations = false;
    Core::NxboxJit::ApplyOptOuts(config);
    assert(config.optimizations == Dynarmic::no_optimizations);
    assert(!config.unsafe_optimizations);
}
int main() { Check<Dynarmic::A32::UserConfig>(); Check<Dynarmic::A64::UserConfig>(); }
"""
        )

    def test_each_opt_out_removes_only_its_flag(self):
        options = {
            "BLOCK_LINKING": "BlockLinking",
            "RSB": "ReturnStackBuffer",
            "FAST_DISPATCH": "FastDispatch",
            "CONTEXT_ELIMINATION": "GetSetElimination",
            "CONST_PROP": "ConstProp",
            "MISC_IR": "MiscIROpt",
            "CODE_SPEED": "CodeSpeed",
            "DISABLE_VERIFICATION": "DisableVerification",
            "UNFUSE_FMA": "Unsafe_UnfuseFMA",
            "REDUCED_ERROR_FP": "Unsafe_ReducedErrorFP",
            "INACCURATE_NAN": "Unsafe_InaccurateNaN",
            "IGNORE_STANDARD_FPCR": "Unsafe_IgnoreStandardFPCRValue",
            "IGNORE_GLOBAL_MONITOR": "Unsafe_IgnoreGlobalMonitor",
        }
        # Compile once, then select each option inside the child process.
        checks = "\n".join(
            f'Check<C>("NXBOX_JIT_{name}", Flag::{flag});' for name, flag in options.items()
        )
        self.run_cpp(
            r"""
#include <cassert>
#include <cstdlib>
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/config.h"
#include "core/arm/dynarmic/nxbox_jit_policy.h"
using Flag = Dynarmic::OptimizationFlag;
template<class C> void Check(const char* name, Flag flag) {
    const auto all = static_cast<Flag>(0x001fffff);
    for (const auto* value : {"", "1", "false", "00", "0"}) {
        setenv(name, value, 1);
        C config{};
        config.optimizations = all;
        Core::NxboxJit::ApplyOptOuts(config);
        assert(config.optimizations == (std::strcmp(value, "0") == 0 ? all & ~flag : all));
        assert(config.global_monitor == nullptr);
    }
    unsetenv(name);
}
template<class C> void All() {
CHECKS
}
int main() { All<Dynarmic::A32::UserConfig>(); All<Dynarmic::A64::UserConfig>(); }
""".replace("CHECKS", checks)
        )

    def test_memory_opt_outs_keep_callback_fallback(self):
        self.run_cpp(
            r"""
#include <cassert>
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/config.h"
#include "core/arm/dynarmic/nxbox_jit_policy.h"
template<class C> void Check() {
    C config{};
    config.page_table = reinterpret_cast<decltype(config.page_table)>(0x1000);
    config.fastmem_pointer = 0x1000;
    config.fastmem_exclusive_access = true;
    config.recompile_on_exclusive_fastmem_failure = true;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    config.callbacks = reinterpret_cast<decltype(config.callbacks)>(0x2000);
    const auto* callbacks = config.callbacks;
    Core::NxboxJit::ApplyOptOuts(config);
    assert(config.page_table == nullptr);
    assert(!config.fastmem_pointer && !config.fastmem_exclusive_access);
    assert(!config.recompile_on_exclusive_fastmem_failure);
    assert(!config.only_detect_misalignment_via_page_table_on_page_boundary);
    assert(config.callbacks == callbacks); // Policy never changes callback identity.
}
int main() { Check<Dynarmic::A32::UserConfig>(); Check<Dynarmic::A64::UserConfig>(); }
""",
            {
                f"NXBOX_JIT_{name}": "0"
                for name in (
                    "PAGE_TABLE",
                    "FASTMEM",
                    "FASTMEM_EXCLUSIVES",
                    "RECOMPILE_EXCLUSIVES",
                    "REDUCE_MISALIGNMENT_CHECKS",
                )
            },
        )

    def test_a64_precise_context_and_fastmem_bounds_opt_outs(self):
        self.run_cpp(
            r"""
#include <cassert>
#include <cstdlib>
#include "dynarmic/interface/A64/config.h"
#include "core/arm/dynarmic/nxbox_jit_policy.h"
int main() {
    Dynarmic::A64::UserConfig config{};
    config.fastmem_address_space_bits = 64;
    Core::NxboxJit::ApplyA64OptOuts(config, 39);
    assert(config.collect_memory_access_context && config.fastmem_address_space_bits == 64);
    setenv("NXBOX_JIT_PRECISE_MEMORY", "0", 1);
    setenv("NXBOX_JIT_UNCHECKED_FASTMEM", "0", 1);
    Core::NxboxJit::ApplyA64OptOuts(config, 39);
    assert(!config.collect_memory_access_context && config.fastmem_address_space_bits == 39);
    // Opt-outs do not enable fastmem on the private UWP backing.
    assert(!config.fastmem_pointer);
}
"""
        )

    def test_profile_intervals_deltas_and_failure_isolation(self):
        self.run_cpp(
            r"""
#include <cassert>
#include <stdexcept>
#include "core/arm/dynarmic/nxbox_jit_profile.h"
using Core::NxboxJit::Profile;
int main() {
    Profile a, b;
    assert(a.enabled && b.enabled);
    const auto start = a.start;
    a.CodeRead(false); a.CodeRead(true); a.PageFill();
    a.MemoryCallback(); a.MemoryCallback();
    unsigned reports = 0;
    auto report = [&](auto reads, auto fills, auto fresh, auto memory, auto interval) {
        assert(reads == 1 && fills == 1 && fresh == 1 && memory == 2 && interval == 5000);
        ++reports;
    };
    a.Poll(report, start + std::chrono::milliseconds(4999));
    assert(reports == 0);
    a.Poll(report, start + std::chrono::seconds(5));
    assert(reports == 1 && a.memory_callbacks == 0 && b.code_reads == 0);
    a.Poll([&](auto reads, auto fills, auto fresh, auto memory, auto interval) {
        assert(reads == 0 && fills == 0 && fresh == 0 && memory == 0 && interval == 7000);
        ++reports;
    }, start + std::chrono::seconds(12));
    a.CodeRead(false);
    a.Poll([](auto...) { throw std::runtime_error("log unavailable"); },
           start + std::chrono::seconds(17));
    assert(a.code_reads == 0 && reports == 2);
    Profile off;
    off.enabled = false;
    off.CodeRead(false); off.CodeRead(true); off.PageFill(); off.MemoryCallback();
    off.Poll([](auto...) { assert(false); }, start + std::chrono::hours(1));
    assert(off.code_reads == 0 && off.fresh_code_reads == 0 && off.memory_callbacks == 0);
}
""",
            {"NXBOX_THREAD_PROFILE": "1"},
        )

    def test_profile_disabled_by_default(self):
        self.run_cpp(
            r"""
#include <cassert>
#include "core/arm/dynarmic/nxbox_jit_profile.h"
int main() {
    Core::NxboxJit::Profile profile;
    assert(!profile.enabled);
    assert(profile.start == Core::NxboxJit::Profile::Clock::time_point{});
    profile.CodeRead(false); profile.CodeRead(true);
    profile.PageFill(); profile.MemoryCallback();
    assert(profile.code_reads == 0 && profile.fresh_code_reads == 0);
    assert(profile.code_page_fills == 0 && profile.memory_callbacks == 0);
}
"""
        )

    def test_disk_cache_second_process_identity_and_corruption(self):
        self.run_cpp(
            r"""
#include <cassert>
#include <cstring>
#include "common/nxbox_jit_cache.h"
using namespace NxboxJitCache;
int main(int argc, char** argv) {
    assert(argc == 3);
    const auto path = std::filesystem::path(argv[1]) / "cache.bin";
    const std::string mode = argv[2];
    Store store(path, 1, [](const auto&) {});
    assert(store.RegisterConfig({1, 2, 3}));
    store.Open({1, 2, 3, 4});
    if (mode == "write") {
        Record r;
        r.descriptor = r.pc = 0x1000;
        r.config = {1, 2, 3};
        r.guest = {0x1f, 0x20, 0x03, 0xd5};
        r.host = {0x90};
        r.relocation = {1, Terminal::Link, 0x1004};
        store.Insert(r);
        store.Flush();
        assert(std::filesystem::file_size(path) <= TitleLimit);
    } else if (mode == "read") {
        auto r = store.Find(0x1000, {1, 2, 3});
        assert(r && r->Valid());
        assert(MatchesGuest(*r, [](auto) { return std::optional<std::uint32_t>{0xd503201f}; }));
        assert(!MatchesGuest(*r, [](auto) { return std::optional<std::uint32_t>{0}; }));
        assert(!store.Find(0x1000, {4}));
        store.Open({5, 6, 7, 8});
        assert(!store.Find(0x1000, {1, 2, 3}));
    } else if (mode == "corrupt") {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte{};
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    } else {
        assert(mode == "reject");
        assert(!store.Find(0x1000, {1, 2, 3}));
    }
}
""",
            runs=(("write",), ("read",), ("corrupt",), ("reject",)),
        )


if __name__ == "__main__":
    unittest.main()
