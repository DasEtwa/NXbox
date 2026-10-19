# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run disk cache serialization, invalidation and cancellable warmup on the host."""

import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <atomic>
#include <fstream>
#include <thread>
#include <set>
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <map>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h"
using namespace D3D12;
int main(int argc, char** argv) {
    assert(argc >= 2);
    if (argc == 3) {
        const std::filesystem::path root = argv[1];
        const std::string mode = argv[2];
        DiskCacheIdentity identity{1, 2, 7, 4, 5, 6, 7, 8, 9};
        if (mode == "no-flush" || mode == "hits-no-flush") {
            // Deliberately leak and _Exit: neither Flush nor any destructor can save the order.
            auto* cache = new DiskPipelineCache(root, 0x9876, identity);
            if (mode == "no-flush") {
                cache->RememberFirstUse(DiskBytes{9});
                cache->RememberFirstUse(DiskBytes{3});
                assert(cache->RememberPipeline({DiskRecordKind::Graphics, 1, {1}, {3}}));
                assert(cache->RememberPipeline({DiskRecordKind::Graphics, 99, {2}, {9}}));
            } else {
                // Only a prewarmed hit this session: no new recipe build or clean teardown.
                cache->RememberFirstUse(DiskBytes{3});
            }
            const auto sidecar = root / "d3d12_cache/0000000000009876/first-use-v7.txt";
            for (unsigned i = 0; i < 500; ++i) {
                std::ifstream file{sidecar};
                const std::string log{std::istreambuf_iterator<char>{file}, {}};
                const auto session = log.rfind("session\n");
                const auto hash = Pso::HashBytes(mode == "no-flush" ? DiskBytes{9}.data() :
                                                DiskBytes{3}.data(), 1);
                std::ostringstream expected;
                expected << "0 " << std::hex << std::setfill('0') << std::setw(16) << hash;
                if (session != std::string::npos && log.find(expected.str(), session) != std::string::npos)
                    std::_Exit(0);
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            std::_Exit(2);
        }
        if (mode == "verify-order" || mode == "verify-hits") {
            DiskPipelineCache cache(root, 0x9876, identity);
            std::vector<std::uint64_t> order;
            cache.StartWarmup([&](const auto& record, auto) { order.push_back(record.key); return true; }, {});
            cache.WaitWarmup();
            assert((order == (mode == "verify-order" ? std::vector<std::uint64_t>{99, 1} :
                                                        std::vector<std::uint64_t>{1, 99})));
            return 0;
        }
        assert(mode == "migrate" || mode == "disabled");
        const auto title = root / "d3d12_cache/0000000000001234";
        const auto read = [](const auto& path) {
            std::ifstream file{path, std::ios::binary};
            return DiskBytes{std::istreambuf_iterator<char>{file}, {}};
        };
        const auto sample = read(std::filesystem::directory_iterator(title)->path());
        const auto u32 = [&](std::size_t offset) {
            return std::uint32_t{sample[offset]} | (std::uint32_t{sample[offset+1]} << 8) |
                   (std::uint32_t{sample[offset+2]} << 16) | (std::uint32_t{sample[offset+3]} << 24);
        };
        identity = {u32(12), u32(16), mode == "migrate" ? 7u : 6u, u32(24), u32(28), u32(32), u32(36)};
        // Recipes/DXIL are portable across driver versions and adapter LUIDs.
        DiskPipelineCache cache(root, 0x1234, identity);
        std::map<std::uint64_t, DiskBytes> dxil;
        std::map<std::uint64_t, DiskPipelineRecord> originals;
        for (const auto& item : std::filesystem::directory_iterator(title)) {
            const auto bytes = read(item.path());
            auto old = identity; old.schema_version = 6;
            const auto record = DeserializeDiskRecord(bytes, old);
            assert(record && !record->legacy);
            if (record->kind == DiskRecordKind::Dxil) {
                dxil.emplace(record->key, bytes);
                assert(cache.FindDxil(record->key) == record->payload);
                assert(read(item.path()) == bytes);
            } else originals.emplace(record->key, *record);
        }
        std::set<DiskBytes> requests;
        unsigned total{}, duplicates{}, compute{};
        cache.StartWarmup([&](const auto& record, auto) {
            ++total;
            assert(record.prewarmed && record.legacy == (mode == "migrate"));
            auto original = originals.at(record.key);
            if (record.kind == DiskRecordKind::Graphics && mode == "migrate") {
                assert(record.legacy_request_hash == Pso::HashBytes(original.request_key.data(), original.request_key.size()));
                for (auto pair : {std::pair{&original.payload, 12u}, std::pair{&original.request_key, 48u}}) {
                    Pso::GraphicsPsoState state;
                    std::memcpy(&state, pair.first->data() + pair.second, sizeof(state));
                    Pso::NormalizeGraphicsState(state);
                    std::memcpy(pair.first->data() + pair.second, &state, sizeof(state));
                }
            } else if (record.kind == DiskRecordKind::Compute) ++compute;
            assert(record.key == original.key && record.payload == original.payload &&
                   record.request_key == original.request_key && record.runtime_data == original.runtime_data);
            if (!requests.insert(record.request_key).second && mode == "migrate") {
                ++duplicates;
                assert(cache.DiscardLegacyDuplicate(record));
            } else assert(cache.RememberPipeline(record));
            return true;
        }, {});
        cache.WaitWarmup();
        assert(total == originals.size() && total > 1000 && !dxil.empty() && compute > 0);
        assert((mode == "migrate") == (duplicates > 0));
        unsigned survivors{};
        for (const auto& item : std::filesystem::directory_iterator(title)) {
            if (item.path().extension() != ".graphics" && item.path().extension() != ".compute") continue;
            const auto record = DeserializeDiskRecord(read(item.path()), identity);
            assert(record && !record->legacy);
            ++survivors;
        }
        assert(survivors == total - duplicates);
        for (const auto& [key, bytes] : dxil) assert(cache.FindDxil(key));
        std::cout << "schema-6 archive: recipes=" << total << " duplicates=" << duplicates
                  << " survivors=" << survivors << " DXIL=" << dxil.size() << " preserved\n";
        return 0;
    }
    const std::filesystem::path root = argv[1];
    DiskCacheIdentity identity{1, 2, 3, 4, 5, 6, 7, 0x1122334455667788, 9};
    DiskPipelineRecord record{DiskRecordKind::Dxil, 0x0123456789abcdef, {0, 255, 1, 2}};
    const auto encoded = SerializeDiskRecord(identity, record);
    assert(encoded.size() == 100 + record.payload.size());
    // Explicit little-endian wire bytes, including full-width keys/LUIDs.
    assert(encoded[12] == 1 && encoded[40] == 0x88 && encoded[47] == 0x11);
    assert(encoded[60] == 0xef && encoded[67] == 0x01 && encoded[68] == 4);
    const auto decoded = DeserializeDiskRecord(encoded, identity);
    assert(decoded && decoded->payload == record.payload && decoded->key == record.key);
    for (auto kind : {DiskRecordKind::Dxil, DiskRecordKind::Graphics,
                      DiskRecordKind::Compute, DiskRecordKind::Library,
                      DiskRecordKind::NativeGraphics, DiskRecordKind::NativeCompute}) {
        auto item = record; item.kind = kind; if (kind == DiskRecordKind::Library) item.key = 0;
        assert(DeserializeDiskRecord(SerializeDiskRecord(identity, item), identity)->kind == kind);
    }
    // Guest identity and draw metadata round-trip independently of the native PSO key.
    for (auto kind : {DiskRecordKind::Graphics, DiskRecordKind::Compute}) {
        DiskPipelineRecord pipeline{kind, record.key, record.payload, {1, 2, 3}, {4, 5}};
        const auto restored = DeserializeDiskRecord(SerializeDiskRecord(identity, pipeline), identity);
        assert(restored && restored->request_key == pipeline.request_key &&
               restored->runtime_data == pipeline.runtime_data && restored->payload == pipeline.payload);
    }
    auto old_version = encoded; old_version[8] = 2;
    assert(!DeserializeDiskRecord(old_version, identity));
    // Every truncated prefix, single-byte corruption, and trailing garbage is a miss.
    for (size_t n = 0; n < encoded.size(); ++n)
        assert(!DeserializeDiskRecord(std::span(encoded).first(n), identity));
    for (size_t n = 0; n < encoded.size(); ++n) {
        auto corrupt = encoded; corrupt[n] ^= 0x80;
        assert(!DeserializeDiskRecord(corrupt, identity));
    }
    // Recompute the checksum to exercise semantic validation independently.
    auto seal = [](DiskBytes& bytes) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (size_t n = 0; n < bytes.size() - 8; ++n) hash = (hash ^ bytes[n]) * 1099511628211ULL;
        for (unsigned n = 0; n < 8; ++n) bytes[bytes.size() - 8 + n] = (hash >> (8 * n)) & 255;
    };
    auto unknown = encoded; unknown[56] = 99; seal(unknown);
    assert(!DeserializeDiskRecord(unknown, identity));
    auto huge = encoded;
    for (unsigned n = 68; n < 76; ++n) huge[n] = 255;
    seal(huge); assert(!DeserializeDiskRecord(huge, identity));
    auto bad_library = encoded; bad_library[56] = 4; seal(bad_library);
    assert(!DeserializeDiskRecord(bad_library, identity));
    auto trailing = encoded; trailing.push_back(0);
    assert(!DeserializeDiskRecord(trailing, identity));
    // The adapter LUID is assigned per boot: a console restart must not discard any record.
    // DXIL and pipeline recipes depend on the emitter, DXC, schema and GPU that produced them;
    // native driver blobs also depend on the driver.
    for (auto kind : {DiskRecordKind::Dxil, DiskRecordKind::Graphics,
                      DiskRecordKind::Compute, DiskRecordKind::Library,
                      DiskRecordKind::NativeGraphics, DiskRecordKind::NativeCompute}) {
        auto item = record; item.kind = kind; if (kind == DiskRecordKind::Library) item.key = 0;
        const auto bytes = SerializeDiskRecord(identity, item);
        const bool portable = kind == DiskRecordKind::Dxil || kind == DiskRecordKind::Graphics ||
                              kind == DiskRecordKind::Compute;
        for (unsigned n = 0; n < 9; ++n) {
            auto stale = identity;
            switch (n) {
            case 0: ++stale.emitter_version; break;
            case 1: ++stale.dxc_version; break;
            case 2: ++stale.schema_version; break;
            case 3: ++stale.vendor_id; break;
            case 4: ++stale.device_id; break;
            case 5: ++stale.subsystem_id; break;
            case 6: ++stale.revision; break;
            case 7: ++stale.adapter_luid; break;
            case 8: ++stale.driver_version; break;
            }
            const bool kept = n == 7 || (portable && n == 8) || (kind == DiskRecordKind::Dxil && n == 2);
            assert(DeserializeDiskRecord(bytes, stale).has_value() == kept);
        }
    }
    for (auto invalid : {DiskPipelineRecord{DiskRecordKind::Dxil, 0, {}},
                         DiskPipelineRecord{static_cast<DiskRecordKind>(99), 0, {1}},
                         DiskPipelineRecord{DiskRecordKind::Library, 1, {1}},
                         DiskPipelineRecord{DiskRecordKind::Compute, 0, DiskBytes((4 << 20) + 1)}}) {
        bool rejected = false;
        try { SerializeDiskRecord(identity, invalid); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    // First-use order is captured before compilation and survives reverse completion.
    {
        DiskPipelineCache cache(root, 0x9876, identity);
        cache.RememberFirstUse(DiskBytes{9});
        cache.RememberFirstUse(DiskBytes{3});
        cache.RememberFirstUse(DiskBytes{9});
        assert(cache.RememberPipeline({DiskRecordKind::Graphics, 1, {1}, {3}}));
        assert(cache.RememberPipeline({DiskRecordKind::Graphics, 99, {2}, {9}}));
        assert(cache.Flush());
    }
    {
        DiskPipelineCache cache(root, 0x9876, identity);
        std::vector<std::uint64_t> order;
        cache.StartWarmup([&](const auto& item, auto) {
            order.push_back(item.key);
            return true;
        }, {});
        cache.WaitWarmup();
        assert((order == std::vector<std::uint64_t>{99, 1}));
    }
    const auto title = root / "d3d12_cache" / "0000000000001234";
    {
        DiskPipelineCache cache(root, 0x1234, identity);
        assert(!cache.FindDxil(record.key));
        assert(cache.StoreDxil(record.key, record.payload));
        assert(cache.FindDxil(record.key) == record.payload);
        const DiskBytes replacement{7, 8};
        assert(cache.StoreDxil(record.key, replacement));
        assert(cache.FindDxil(record.key) == replacement);
        assert(!cache.StoreDxil(record.key, {}));
        assert(cache.FindDxil(record.key) == replacement);
        // Failed replacement preserves the last complete record.
        const auto blocked_temp = title / "0123456789abcdef.dxil.tmp";
        std::filesystem::create_directory(blocked_temp);
        std::ofstream(blocked_temp / "blocker") << "occupied";
        assert(!cache.StoreDxil(record.key, record.payload));
        assert(cache.FindDxil(record.key) == replacement);
        std::filesystem::remove_all(blocked_temp);
        assert(!cache.RememberPipeline(record));
        assert(cache.RememberPipeline({DiskRecordKind::Graphics, 1, {1}}));
        assert(cache.RememberPipeline({DiskRecordKind::Compute, 2, {2}}));
    }
    {
        DiskPipelineCache cache(root, 0x1234, identity);
        assert(cache.FindDxil(record.key) == DiskBytes({7, 8}));
        // Reject oversized files before allocating their declared payload.
        const auto oversized = title / "0000000000000004.dxil";
        std::ofstream(oversized).close();
        std::filesystem::resize_file(oversized, (64ULL << 20) + 101);
        assert(!cache.FindDxil(4) && !std::filesystem::exists(oversized));
        // Ensure renamed files cannot serve the wrong shader key.
        std::filesystem::copy_file(title / "0123456789abcdef.dxil", title / "0000000000000003.dxil");
        assert(!cache.FindDxil(3));
        assert(!std::filesystem::exists(title / "0000000000000003.dxil"));
        std::atomic<bool> finished{};
        std::vector<DiskWarmupProgress> progress;
        cache.StartWarmup([&](const auto& item, std::stop_token) {
            assert(cache.FindDxil(record.key)); // Callback may reenter the cache.
            return item.kind == DiskRecordKind::Graphics;
        }, [&](const auto& item) { progress.push_back(item); if (item.stage == DiskWarmupStage::Complete) finished = true; });
        cache.WaitWarmup();
        assert(finished);
        assert(progress.size() == 4 && progress.front().completed == 0);
        assert(progress.back().completed == 2 && progress.back().total == 2 && progress.back().failed == 1);
        assert(progress[1].completed == 1 && progress[2].completed == 2);
        // A failed build is isolated and subsequent records still execute.
        finished = false;
        cache.StartWarmup([](const auto&, std::stop_token) -> bool { throw std::runtime_error("rejected"); },
                          [&](const auto& p) { if (p.stage == DiskWarmupStage::Complete) { assert(p.failed == 2); finished = true; } });
        while (!finished) std::this_thread::yield();
        cache.StopWarmup();
        // Cancellation reaches a currently running build, not just the next record.
        std::stop_source external;
        std::atomic<bool> entered{}, cancelled{};
        cache.StartWarmup([&](const auto&, std::stop_token stop) {
            entered = true;
            while (!stop.stop_requested()) std::this_thread::yield();
            return false;
        }, [&](const auto& p) { if (p.stage == DiskWarmupStage::Cancelled) cancelled = true; }, external.get_token());
        while (!entered) std::this_thread::yield();
        external.request_stop();
        cache.StopWarmup();
        assert(cancelled);
        entered = false; cancelled = false;
        cache.StartWarmup([&](const auto&, std::stop_token stop) {
            entered = true; while (!stop.stop_requested()) std::this_thread::yield(); return false;
        }, [&](const auto& p) { if (p.stage == DiskWarmupStage::Cancelled) cancelled = true; });
        while (!entered) std::this_thread::yield();
        cache.StopWarmup();
        assert(cancelled);
        // A pre-cancelled external token does not start any builds.
        cancelled = false;
        cache.StartWarmup([](const auto&, std::stop_token) { assert(false); return false; },
                          [&](const auto& p) { if (p.stage == DiskWarmupStage::Cancelled) cancelled = true; }, external.get_token());
        cache.StopWarmup();
        assert(cancelled);
        // Corrupt recipes are removed without losing independently valid DXIL.
        std::ofstream(title / "0000000000000001.graphics", std::ios::binary) << "broken";
        finished = false;
        cache.StartWarmup([](const auto&, std::stop_token) { return true; }, [&](const auto& p) {
            if (p.stage == DiskWarmupStage::Complete) { assert(p.failed == 1); finished = true; }
        });
        while (!finished) std::this_thread::yield();
        cache.StopWarmup();
        assert(!std::filesystem::exists(title / "0000000000000001.graphics"));
        assert(cache.FindDxil(record.key));
    }
    {
        auto stale = identity; ++stale.emitter_version;
        DiskPipelineCache cache(root, 0x1234, stale);
        assert(!cache.FindDxil(record.key));
        assert(!std::filesystem::exists(title / "0123456789abcdef.dxil"));
        assert(cache.StoreDxil(record.key, record.payload));
    }
    {
        DiskPipelineCache other(root, 0x5678, identity);
        assert(!other.FindDxil(record.key));
        std::atomic<bool> finished{};
        other.StartWarmup([](const auto&, std::stop_token) { assert(false); return false; }, [&](const auto& p) {
            if (p.stage == DiskWarmupStage::Complete) { assert(p.total == 0 && p.completed == 0); finished = true; }
        });
        while (!finished) std::this_thread::yield();
        other.StopWarmup();
        DiskPipelineCache disabled(root, 0, identity);
        assert(!disabled.StoreDxil(1, record.payload));
        assert(!std::filesystem::exists(root / "d3d12_cache" / "0000000000000000"));
        // An unavailable directory makes persistence optional, not a boot failure.
        const auto blocked = root / "blocked";
        std::ofstream(blocked) << "file";
        DiskPipelineCache unavailable(blocked, 1, identity);
        assert(!unavailable.StoreDxil(1, record.payload));
    }
}
"""


class D3D12DiskPipelineCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-d3d12-disk-")
        source = Path(cls.temp.name) / "disk.cpp"
        cls.binary = Path(cls.temp.name) / "disk"
        source.write_text(DRIVER)
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-pthread",
                "-I",
                str(ROOT / "src"),
                str(source),
                str(ROOT / "src/video_core/renderer_d3d12/d3d12_disk_pipeline_cache.cpp"),
                "-o",
                str(cls.binary),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_serialization_and_warmup(self):
        subprocess.run([str(self.binary), self.temp.name], check=True, timeout=30)

    def test_first_use_survives_exit_without_flush_and_prewarmed_only_hits(self):
        order_root = str(Path(self.temp.name) / "no-flush")
        for mode in ("no-flush", "verify-order", "hits-no-flush", "verify-hits"):
            subprocess.run([str(self.binary), order_root, mode], check=True, timeout=30)
        print("first-use order survives _Exit without Flush, including prewarmed-only hits")

    def test_real_schema6_archive_migration_and_disabled_escape_hatch(self):
        archive = os.environ.get("NXBOX_PSO_CENSUS_DIR")
        if not archive:
            self.skipTest("set NXBOX_PSO_CENSUS_DIR to the read-only c24 artifact folder")
        for mode in ("migrate", "disabled"):
            destination = Path(self.temp.name) / mode / "d3d12_cache/0000000000001234"
            destination.mkdir(parents=True)
            for source_file in (Path(archive) / "mk8d-orig").iterdir():
                if (
                    source_file.suffix in (".graphics", ".compute", ".dxil")
                    and struct.unpack_from("<I", source_file.read_bytes(), 8)[0] == 3
                ):
                    shutil.copyfile(source_file, destination / source_file.name)
            subprocess.run(
                [str(self.binary), str(Path(self.temp.name) / mode), mode],
                check=True,
                timeout=120,
                env={**os.environ, "NXBOX_D3D12_PSO_NORMALIZE": "0" if mode == "disabled" else "1"},
            )
