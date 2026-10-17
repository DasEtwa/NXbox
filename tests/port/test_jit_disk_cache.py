# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the std-only production cache codec, relocation and guest guards."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class JitDiskCacheTests(unittest.TestCase):
    def test_emitter_admission_rejects_hidden_process_dependencies(self):
        emitter = ROOT / "src/dynarmic/src/dynarmic/backend/x64/a64_emit_x64.cpp"
        method = re.search(
            r"^bool A64EmitX64::DiskCacheEligible\(.*?^\}",
            emitter.read_text(),
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(method)
        program = r"""
#include <cassert>
#include <variant>
#include <vector>
namespace IR {
enum class Opcode {
#define OPCODE(name, ...) name,
#define A32OPC(name, ...) A32##name,
#define A64OPC(name, ...) A64##name,
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
};
enum class Cond { AL, NE };
namespace Term {
struct Link {};
using LeafTerminal = std::variant<std::monostate, Link>;
using Terminal = std::variant<std::monostate, LeafTerminal, int>;
}
struct Inst { Opcode opcode; Opcode GetOpcode() const { return opcode; } };
struct Block {
    bool step = false;
    Cond condition = Cond::AL;
    std::vector<Inst> instructions;
    Term::Terminal terminal = Term::LeafTerminal{Term::Link{}};
    bool Location() const { return step; }
    Cond GetCondition() const { return condition; }
};
}
namespace A64 {
struct LocationDescriptor {
    bool step;
    bool SingleStepping() const { return step; }
};
}
struct A64EmitX64 { static bool DiskCacheEligible(const IR::Block&); };
METHOD
int main() {
    IR::Block block;
    assert(A64EmitX64::DiskCacheEligible(block));
    block.instructions = {{IR::Opcode::A64GetX}, {IR::Opcode::Add64}, {IR::Opcode::A64SetX}};
    assert(A64EmitX64::DiskCacheEligible(block));
    for (const auto op : {IR::Opcode::A64ReadMemory64, IR::Opcode::A64WriteMemory32,
                         IR::Opcode::A64GetTPIDR, IR::Opcode::A64SetFPCR,
                         IR::Opcode::A64CallSupervisor, IR::Opcode::CallHostFunction,
                         IR::Opcode::PushRSB, IR::Opcode::VectorOr,
                         static_cast<IR::Opcode>(-1)}) {
        block.instructions.push_back({op});
        assert(!A64EmitX64::DiskCacheEligible(block));
        block.instructions.pop_back();
    }
    block.step = true;
    assert(!A64EmitX64::DiskCacheEligible(block));
    block.step = false;
    block.condition = IR::Cond::NE;
    assert(!A64EmitX64::DiskCacheEligible(block));
    block.condition = IR::Cond::AL;
    block.terminal = 1; // Conditional or check-halt terminal is not a leaf.
    assert(!A64EmitX64::DiskCacheEligible(block));
    block.terminal = IR::Term::LeafTerminal{};
    assert(!A64EmitX64::DiskCacheEligible(block));
}
""".replace("METHOD", method.group())
        with tempfile.TemporaryDirectory(prefix="nxbox-jit-admission-") as directory:
            directory = Path(directory)
            source = directory / "check.cpp"
            source.write_text(program)
            binary = directory / "check"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'src/dynarmic/src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_codec_relocation_storage_and_fresh_guest_validation(self):
        program = r"""
#include <cassert>
#include <atomic>
#include <thread>
#include "common/nxbox_jit_cache.h"
using namespace NxboxJitCache;
static Record MakeRecord() {
    Record r;
    r.descriptor = r.pc = 0x1000;
    r.config = {1, 2, 3};
    r.guest = {0x1f, 0x20, 0x03, 0xd5, 0, 0, 0, 0x14};
    r.host = {0x90, 0x90};
    r.relocation = {2, Terminal::Link, 0x1008};
    return r;
}
static Bytes ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
static void WriteFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
int main(int argc, char** argv) {
    assert(argc == 2);
    auto r = MakeRecord();
    assert(r.Valid());
    const auto encoded = EncodeRecord(r);
    Record decoded;
    assert(DecodeRecord(encoded, decoded));
    assert(decoded.Matches(r.guest));
    auto changed = r.guest;
    changed.back() ^= 1;
    assert(!decoded.Matches(changed));
    assert(!decoded.Matches(std::span(r.guest).first(4)));
    // Fresh checks observe a write even if a frontend instruction-page snapshot is stale.
    std::uint32_t words[] = {0xd503201f, 0x14000000};
    const auto fresh = [&](std::uint64_t pc) -> std::optional<std::uint32_t> {
        assert(pc == 0x1000 || pc == 0x1004);
        return words[(pc - 0x1000) / 4];
    };
    assert(MatchesGuest(r, fresh));
    words[1] ^= 1;
    assert(!MatchesGuest(r, fresh));
    words[1] ^= 1;
    assert(!MatchesGuest(r, [](auto) -> std::optional<std::uint32_t> { return {}; }));
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        assert(!DecodeRecord(std::span(encoded).first(length), decoded));
    }
    auto trailing = encoded;
    trailing.push_back(0);
    assert(!DecodeRecord(trailing, decoded));
    auto corrupt = encoded;
    corrupt[23] = 0xff; // Config length exceeds bounds.
    assert(!DecodeRecord(corrupt, decoded));
    Bytes emitted;
    std::uint64_t runtime_target = 0xaaaabbbb;
    unsigned terminals = 0;
    auto relocate = [&](const Record& entry) {
        return Relocate(entry,
            [&](const Bytes& body) { emitted.insert(emitted.end(), body.begin(), body.end()); },
            [&](const Relocation& rel) {
                assert(rel.offset == 2 && rel.target == 0x1008 && rel.terminal == Terminal::Link);
                ++terminals;
                Put(emitted, runtime_target);
            });
    };
    assert(relocate(r));
    const auto process_a = emitted;
    emitted.clear();
    runtime_target = 0xccccdddd;
    assert(relocate(r));
    assert(process_a != emitted && terminals == 2);
    r.relocation.offset = 1;
    emitted.clear();
    assert(!relocate(r) && emitted.empty() && terminals == 2);
    r = MakeRecord();
    r.relocation.terminal = static_cast<Terminal>(99);
    assert(!relocate(r));
    r = MakeRecord();
    r.pc = UINT64_MAX - 3;
    assert(!r.Valid());
    r = MakeRecord();
    r.guest.resize(GuestLimit + 4);
    assert(!r.Valid());
    r = MakeRecord();

    const auto path = std::filesystem::path(argv[1]) / "0100000000000000.bin";
    std::vector<std::string> logs;
    auto report = [&](const std::string& line) { logs.push_back(line); };
    {
        Store store(path, 1, report);
        store.Open({1, 2, 3, 4});
        store.Insert(r);
        std::vector<std::thread> workers;
        for (unsigned core = 0; core < 4; ++core) {
            workers.emplace_back([&, core] {
                auto entry = r;
                for (unsigned i = 0; i < 100; ++i) {
                    entry.descriptor = 0x2000 + core * 400 + i * 4;
                    store.Insert(entry);
                    assert(store.Find(entry.descriptor, entry.config));
                    store.Result(true, entry.host.size());
                }
            });
        }
        for (auto& worker : workers) worker.join();
        store.Flush();
    }
    assert(std::filesystem::file_size(path) < TitleLimit);
    assert(logs.back().find("hits=400") != std::string::npos);
    assert(logs.back().find("bytes=800") != std::string::npos);
    const auto original = ReadFile(path);
    {
        Store store(path, 1, report);
        assert(store.RegisterConfig({9, 9}));
        store.Open({1, 2, 3, 4}); // Drop obsolete emitter/config keys before admission hits the cap.
        assert(!store.Find(r.descriptor, r.config));
        auto new_config = r;
        new_config.config = {9, 9};
        store.Insert(new_config);
        store.Flush();
        assert(store.Find(r.descriptor, new_config.config));
    }
    WriteFile(path, original);
    {
        Store store(path, 1, report);
        store.Open({1, 2, 3, 4});
        assert(store.Find(r.descriptor, r.config));
        assert(!store.Find(r.descriptor, {9}));
        store.Open({1, 2, 3, 5}); // New NSO identity rejects the old cache.
        assert(!store.Find(r.descriptor, r.config));
    }
    for (const auto length : {std::size_t{0}, original.size() / 2, original.size() - 1}) {
        WriteFile(path, Bytes(original.begin(), original.begin() + length));
        Store store(path, 1, report);
        store.Open({1, 2, 3, 4});
        assert(!store.Find(r.descriptor, r.config));
    }
    auto broken = original;
    broken.back() ^= 1; // Whole file discarded, including earlier valid records.
    WriteFile(path, broken);
    {
        Store store(path, 1, report);
        store.Open({1, 2, 3, 4});
        assert(!store.Find(r.descriptor, r.config));
        store.Insert(r);
        store.Flush(); // Replace a corrupt file with a valid snapshot.
    }
    {
        Store store(path, 1, report);
        store.Open({1, 2, 3, 4});
        assert(store.Find(r.descriptor, r.config));
        store.Open({}); // Missing executable build identity fails closed.
        assert(!store.Find(r.descriptor, r.config));
    }
    const auto capped_path = std::filesystem::path(argv[1]) / "capped.bin";
    {
        Store store(capped_path, 2, report);
        store.Open({2});
        r.host.resize(HostLimit - 1, 0x90);
        r.relocation.offset = r.host.size();
        for (unsigned i = 0; i < 400; ++i) {
            r.descriptor = i * 4;
            store.Insert(r);
        }
        store.Flush();
        assert(std::filesystem::file_size(capped_path) <= TitleLimit);
        assert(!store.Find(399 * 4, r.config));
    }
    // Directory cap: sparse old files require no large test allocation.
    for (unsigned i = 0; i < 8; ++i) {
        const auto old = std::filesystem::path(argv[1]) / ("old" + std::to_string(i) + ".bin");
        WriteFile(old, {});
        std::filesystem::resize_file(old, TitleLimit);
        std::filesystem::last_write_time(old, std::filesystem::file_time_type::clock::now() -
                                                std::chrono::hours(1));
    }
    {
        Store store(path, 1, report);
        store.Open({1, 2, 3, 4});
        store.Insert(MakeRecord());
        store.Flush();
    }
    std::uintmax_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
        if (entry.path().extension() == ".bin") total += entry.file_size();
    }
    assert(total <= TotalLimit && std::filesystem::exists(path));
    // Unwritable/invalid destination is a harmless persistence failure.
    {
        Store store(path / "not-a-directory.bin", 1, report);
        store.Open({1});
        store.Insert(MakeRecord());
        store.Flush();
    }
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-jit-disk-") as directory:
            directory = Path(directory)
            source = directory / "check.cpp"
            source.write_text(program)
            binary = directory / "check"
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
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            storage = directory / "cache"
            storage.mkdir()
            subprocess.run([str(binary), str(storage)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
