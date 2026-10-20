# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production root-binding batches across state changes and chunk boundaries."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class RootBindingBatchTests(unittest.TestCase):
    def test_draw_uses_batch_with_immediate_and_bisect_fallbacks(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        draw = source.split("void RasterizerD3D12::PrepareDraw(", 1)[1].split(
            "void RasterizerD3D12::Draw(", 1
        )[0]
        self.assertTrue('BisectEnabled("NXBOX_D3D12_NO_ROOT_BINDING_BATCH")' in draw)
        self.assertIn("commands->stream && root_binding_batch", draw)
        self.assertIn("RecordGraphicsRootBindings<RecordingTarget>", draw)
        self.assertLess(
            draw.index("RecordGraphicsRootBindings<RecordingTarget>"),
            draw.index("commands->OMSetRenderTargets"),
        )
        self.assertIn("recorded.root_args.tables.Update(parameter, handle.ptr)", draw)
        self.assertIn(
            "recorded.root_args.cbvs.Update(kFirstRootCbv + i, root_cbv_addresses[i])", draw
        )

    def test_production_batch_matches_individual_binds(self):
        program = r"""
#include <cassert>
#include <iostream>
#include <thread>
#include <tuple>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"
using namespace D3D12;
struct Handle { std::uint64_t ptr; };
struct List {
    std::vector<std::tuple<bool, unsigned, std::uint64_t>> calls;
    void SetGraphicsRootDescriptorTable(unsigned parameter, Handle handle) {
        calls.emplace_back(true, parameter, handle.ptr);
    }
    void SetGraphicsRootConstantBufferView(unsigned parameter, std::uint64_t address) {
        calls.emplace_back(false, parameter, address);
    }
};
struct Target { List* list; };
struct SizeStream {
    size_t commands = 0, bytes = 0;
    template <class Command> void Begin(size_t extra) {
        ++commands;
        bytes += sizeof(RecordStream<>::Header);
        assert(sizeof(Command) + extra > 0);
    }
    void Write(const void*, size_t size) { bytes += size; }
};
int main() {
    // Headers and variable snapshots cross tiny chunks, including mid-address.
    RecordStream<71, 2> stream;
    List expected, actual;
    Target target{&actual};
    std::thread recorder([&] { while (stream.Consume(&target)) {} });
    RecordedSlots<std::uint64_t, 64> tables, cbvs;
    size_t individual_calls = 0, batches = 0;
    for (unsigned draw = 0; draw < 2000; ++draw) {
        if (draw % 13 == 0) { // Root-signature/list/heap invalidation.
            tables.valid = 0;
            cbvs.valid = 0;
        }
        std::array<RecordedRootBinding, 64> bindings{};
        size_t count = 0;
        for (unsigned parameter = 1; parameter < 4; ++parameter) {
            const std::uint64_t address = (std::uint64_t{1} << 40) + draw / 7 + parameter;
            if (tables.Update(parameter, address)) {
                bindings[count++] = {address, parameter, true};
                expected.SetGraphicsRootDescriptorTable(parameter, {address});
            }
        }
        for (unsigned parameter = 4; parameter < 64; ++parameter) {
            const std::uint64_t address = (std::uint64_t{1} << 48) + draw / 3 + parameter;
            if (cbvs.Update(parameter, address)) {
                bindings[count++] = {address, parameter, false};
                expected.SetGraphicsRootConstantBufferView(parameter, address);
            }
        }
        RecordGraphicsRootBindings<Target>(stream, bindings.data(), count);
        batches += count != 0;
        individual_calls += count;
        // Reusing producer scratch must not affect the immutable batch.
        bindings = {};
    }
    stream.Close();
    recorder.join();
    assert(actual.calls == expected.calls);
    assert(batches < individual_calls / 20);
    // Empty batches must not create a command, including null/zero snapshots.
    RecordStream<71, 2> empty;
    RecordGraphicsRootBindings<Target>(empty, nullptr, 0);
    empty.Close();
    assert(!empty.Consume(&target));
    // Failure drain consumes encoded bytes without issuing any D3D calls.
    RecordStream<> abandoned;
    const RecordedRootBinding binding{123, 63, false};
    RecordGraphicsRootBindings<Target>(abandoned, &binding, 1);
    abandoned.Close();
    const auto prior = actual.calls.size();
    assert(abandoned.Consume(&target, false));
    assert(!abandoned.Consume(&target));
    assert(actual.calls.size() == prior);
    // Representative BotW draw: three tables and five CBVs. Compare actual
    // EncodeCall layouts, including headers, call alignment and snapshot padding.
    const std::array<RecordedRootBinding, 8> typical{{
        {100, 1, true}, {200, 2, true}, {300, 3, true},
        {400, 4, false}, {500, 5, false}, {600, 6, false},
        {700, 7, false}, {800, 8, false}}};
    SizeStream separate, batched;
    for (const auto& root : typical) {
        if (root.table) {
            EncodeCall<[](void* output, unsigned parameter, Handle handle) {
                static_cast<Target*>(output)->list->SetGraphicsRootDescriptorTable(parameter, handle);
            }>(separate, root.parameter, Handle{root.address});
        } else {
            EncodeCall<[](void* output, unsigned parameter, std::uint64_t address) {
                static_cast<Target*>(output)->list->SetGraphicsRootConstantBufferView(parameter, address);
            }>(separate, root.parameter, root.address);
        }
    }
    RecordGraphicsRootBindings<Target>(batched, typical.data(), typical.size());
    assert(separate.commands == 8 && batched.commands == 1);
    assert(batched.bytes < separate.bytes);
    std::cout << "3 tables + 5 CBVs: commands=" << separate.commands << "->" << batched.commands
              << " bytes=" << separate.bytes << "->" << batched.bytes << "\n";
    std::cout << individual_calls << " individual binds matched " << batches
              << " batches; empty and abandoned batches verified\n";
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-root-binding-batch-") as directory:
            source = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source.write_text(program)
            build = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-fsanitize=address,undefined",
                    "-pthread",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
