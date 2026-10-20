# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production x64 allocator bookkeeping against the old exhaustive lookup.

Emission is stubbed; these tests verify metadata, not x86 execution. The optional
--benchmark prints sparse-allocation lookup/scope timings, not a game speedup.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/dynarmic/src/dynarmic/backend/x64"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def program():
    header = (BACKEND / "reg_alloc.h").read_text()
    source = (BACKEND / "reg_alloc.cpp").read_text()
    info = header[
        header.index("struct HostLocInfo {") : header.index("//static_assert(sizeof(HostLocInfo)")
    ]
    info = info.replace("private:", "public:")
    members = re.search(r"    static constexpr size_t LocationWords.*?#endif", header, re.S)[0]
    inline = "\n".join(
        function(header, sig)
        for sig in (
            "inline void EndOfAllocScope()",
            "inline void AssertNoMoreUses()",
            "void ReportProfile()",
            "inline HostLocInfo& LocInfo(",
            "inline const HostLocInfo& LocInfo(",
        )
    )
    resets = []
    for backend in ("a64_emit_x64.cpp", "a32_emit_x64.cpp"):
        emit = (BACKEND / backend).read_text()
        start = emit.index("    this->reg_alloc.~RegAlloc();")
        end = emit.index("EmitContext ctx", start)
        resets.append(emit[start : emit.rfind("\n", start, end)])
    methods = "\n".join(
        function(source, sig)
        for sig in (
            "void HostLocInfo::ReleaseOne()",
            "void HostLocInfo::ReleaseAll()",
            "void HostLocInfo::AddValue(",
            "std::optional<HostLoc> RegAlloc::ValueLocation(",
            "void RegAlloc::Move(",
            "void RegAlloc::Exchange(",
        )
    )
    width = function(source, "static inline size_t GetBitWidth(")
    return (
        r"""
#include <algorithm>
#include <array>
#include <bitset>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>
#include "common/nxbox_stall.h"
#define ASSERT(x) assert(x)
#define DEBUG_ASSERT(x) assert(x)
#define UNREACHABLE() std::abort()
using u64 = std::uint64_t;
namespace boost::container {
inline int live_vectors = 0;
template<class T, int N> struct small_vector : std::vector<T> {
    small_vector() { ++live_vectors; }
    small_vector(const small_vector& other) : std::vector<T>(other) { ++live_vectors; }
    small_vector(small_vector&& other) noexcept : std::vector<T>(std::move(other)) { ++live_vectors; }
    small_vector& operator=(small_vector&&) = default;
    ~small_vector() { --live_vectors; }
};
}
namespace IR {
enum class Type { U1, U8, U16, U32, U64, U128, NZCVFlags };
struct Inst {
    unsigned uses = 4;
    unsigned UseCount() const { return uses; }
    Type GetType() const { return Type::U64; }
};
}
enum class HostLoc : unsigned { RAX = 0, RSP = 4, R13 = 13, R14 = 14, R15 = 15, FirstSpill = 38 };
constexpr auto ABI_JIT_PTR = HostLoc::R15;
constexpr size_t NonSpillHostLocCount = 38, SpillCount = 64;
size_t HostLocBitWidth(HostLoc) { return 128; }
struct BlockOfCode {};
@WIDTH@
@INFO@
class RegAlloc {
public:
    RegAlloc(std::bitset<32> = {}, std::bitset<32> = {}) {}
    std::array<HostLocInfo, NonSpillHostLocCount + SpillCount> hostloc_info;
@MEMBERS@
@INLINE@
    std::optional<HostLoc> ValueLocation(const IR::Inst*) const noexcept;
    void Move(BlockOfCode&, HostLoc, HostLoc) noexcept;
    void Exchange(BlockOfCode&, HostLoc, HostLoc) noexcept;
    void EmitMove(BlockOfCode&, size_t, HostLoc, HostLoc) {}
    void EmitExchange(BlockOfCode&, HostLoc, HostLoc) {}
    std::optional<HostLoc> SlowLookup(const IR::Inst* inst) const {
        for (size_t i = 0; i < hostloc_info.size(); ++i)
            if (hostloc_info[i].ContainsValue(inst)) return HostLoc(i);
        return {};
    }
    void SlowEndScope() { for (auto& info : hostloc_info) info.ReleaseAll(); }
};
@METHODS@
struct Emitter {
    RegAlloc reg_alloc;
    struct { std::optional<int> fastmem_pointer; bool page_table = false; } conf;
    std::bitset<32> any_gpr{0xffff}, any_xmm{0xffff0000};
    void ResetA64() {
@RESET64@
    }
    void ResetA32() {
@RESET32@
    }
};
int main(int argc, char**) {
    {
        Emitter emitter;
        IR::Inst aliases[16];
        auto baseline = boost::container::live_vectors;
        for (int i = 0; i < 100; ++i) {
            for (auto& alias : aliases)
                emitter.reg_alloc.LocInfo(HostLoc(1)).AddValue(HostLoc(1), &alias);
            emitter.ResetA64();
            assert(boost::container::live_vectors == baseline);
            emitter.ResetA32();
            assert(boost::container::live_vectors == baseline);
        }
    }
    assert(boost::container::live_vectors == 0);
    BlockOfCode code;
    RegAlloc alloc;
    std::array<IR::Inst, 256> values;
    auto check = [&] {
        for (auto& value : values)
            assert(alloc.ValueLocation(&value) == alloc.SlowLookup(&value));
    };
    auto end_scope = [&] {
        auto reference = alloc;
        reference.SlowEndScope();
        alloc.EndOfAllocScope();
        for (size_t i = 0; i < alloc.hostloc_info.size(); ++i) {
            const auto& actual = alloc.hostloc_info[i];
            const auto& expected = reference.hostloc_info[i];
            assert(actual.values == expected.values);
            assert(actual.current_references == expected.current_references);
            assert(actual.accumulated_uses == expected.accumulated_uses);
            assert(actual.total_uses == expected.total_uses);
            assert(actual.is_being_used_count == expected.is_being_used_count);
            assert(actual.is_scratch == expected.is_scratch);
            assert(actual.is_set_last_use == expected.is_set_last_use);
            assert(actual.max_bit_width == expected.max_bit_width);
        }
    };
    // Live aliases, a spill in each mask word, move and exchange before scope end.
    alloc.LocInfo(HostLoc(1)).AddValue(HostLoc(1), &values[0]);
    alloc.LocInfo(HostLoc(1)).AddValue(HostLoc(1), &values[1]);
    alloc.LocInfo(HostLoc(101)).AddValue(HostLoc(101), &values[2]);
    check(); end_scope(); check();
    alloc.Move(code, HostLoc(64), HostLoc(1)); check();
    alloc.Exchange(code, HostLoc(64), HostLoc(101)); check();
    end_scope(); check();
    // Partial ReleaseOne, final use, and last-use reuse by a new alias.
    auto& last = alloc.LocInfo(HostLoc(31));
    values[3].uses = 1;
    last.AddValue(HostLoc(31), &values[3]);
    last.AddArgReference(); last.ReadLock(); last.ReleaseOne();
    check();
    last.AddValue(HostLoc(31), &values[4]);
    for (int i = 0; i < 4; ++i) last.AddArgReference();
    last.is_set_last_use = true;
    last.AddValue(HostLoc(31), &values[5]); check();
    end_scope(); check();
    // Randomized definitions, aliases, locks, pending uses, releases and movement.
    std::mt19937 random(123456);
    for (int step = 0; step < 20000; ++step) {
        auto loc = HostLoc(random() % 102);
        if (loc == HostLoc::RSP || loc == ABI_JIT_PTR) continue;
        auto& info = alloc.LocInfo(loc);
        auto& value = values[random() % values.size()];
        if (!alloc.ValueLocation(&value) && !info.IsLocked()) {
            value.uses = 1 + random() % 4;
            info.AddValue(loc, &value);
        }
        if (!info.values.empty() && info.current_references + info.accumulated_uses < info.total_uses) {
            info.AddArgReference(); info.ReadLock();
            if (random() % 2) info.ReleaseOne();
        }
        const auto other = HostLoc(random() % 102);
        if (other != HostLoc::RSP && other != ABI_JIT_PTR && other != loc) {
            const auto& other_info = std::as_const(alloc).LocInfo(other);
            if (!info.IsLocked() && !other_info.IsLocked() &&
                (!info.IsEmpty() || !other_info.IsEmpty())) {
                alloc.Exchange(code, loc, other);
            }
        }
        if (random() % 3 == 0) end_scope();
        check();
    }
    // Every pending reference is applied once, and untouched live aliases survive.
    end_scope(); check();
#if NXBOX_STALL_PROFILE
    const auto releases = alloc.scope_releases;
#endif
    end_scope();
#if NXBOX_STALL_PROFILE
    assert(alloc.scope_releases == releases);
    alloc.ReportProfile();
    assert(NxboxStall::TakeJit(NxboxStall::JitEvent::RegAllocLookups) == alloc.value_lookups);
    assert(NxboxStall::TakeJit(NxboxStall::JitEvent::RegAllocProbes) == alloc.value_probes);
    assert(NxboxStall::TakeJit(NxboxStall::JitEvent::RegAllocReleases) == releases);
#endif
    if (argc > 1) {
        RegAlloc sparse;
        sparse.LocInfo(HostLoc(1)).AddValue(HostLoc(1), &values[0]);
        sparse.EndOfAllocScope();
        auto bench = [&](bool fast) {
            auto start = std::chrono::steady_clock::now();
            unsigned found = 0;
            for (unsigned i = 0; i < 2000000; ++i) {
                // Missing definitions are the costly successful assertion path.
                found += bool(fast ? sparse.ValueLocation(&values[i % 256])
                                   : sparse.SlowLookup(&values[i % 256]));
                sparse.LocInfo(HostLoc(1)).ReadLock();
                if (fast) sparse.EndOfAllocScope(); else sparse.SlowEndScope();
            }
            std::cout << (fast ? "indexed" : "exhaustive") << " us="
                      << std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start).count()
                      << " found=" << found << '\n';
        };
        bench(false); bench(true);
    }
}
""".replace("@WIDTH@", width)
        .replace("@INFO@", info)
        .replace("@MEMBERS@", members)
        .replace("@INLINE@", inline)
        .replace("@METHODS@", methods)
        .replace("@RESET64@", resets[0])
        .replace("@RESET32@", resets[1])
    )


class RegAllocTests(unittest.TestCase):
    def test_index_matches_exhaustive_lookup(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-regalloc-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(program())
            for profile in (0, 1):
                subprocess.run(
                    [
                        *shlex.split(os.environ.get("CXX", "c++")),
                        "-std=c++20",
                        "-O2",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-Wno-unused-parameter",
                        f"-DNXBOX_STALL_PROFILE={profile}",
                        f"-I{ROOT / 'src'}",
                        str(cpp),
                        "-o",
                        str(binary),
                    ],
                    check=True,
                )
                subprocess.run(
                    [
                        str(binary),
                        *(["benchmark"] if "--benchmark" in sys.argv and profile else []),
                    ],
                    check=True,
                    timeout=30,
                )


if __name__ == "__main__":
    unittest.main(argv=[arg for arg in sys.argv if arg != "--benchmark"])
