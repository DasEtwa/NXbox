# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep vertex page-watch snapshots read-only, trace-gated, and tied to both endpoints."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PageWatchTests(unittest.TestCase):
    def test_source_trace_snapshots_both_pages_before_completion(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        block = source.split("const u64 copied = std::min(available, required);", 1)[1].split(
            "auto zero = context.CreateBuffer(required", 1
        )[0]
        self.assertIn("if (trace_vertex_bytes)", block)
        self.assertIn("trace_page(copied ? device_address : std::nullopt)", block)
        self.assertIn("gpu_memory->GpuToCpuAddress(guest_address + copied - 1)", block)
        self.assertIn("device_memory.GetPageCacheInfo(*address)", block)
        self.assertIn("info->memory->GetPageEntryInfo(info->cpu_vaddr)", block)
        snapshot, callback = block.split("context.AfterCompletion(", 1)
        self.assertIn("const auto last_page =", snapshot)
        self.assertIn("cpu_modified, first_page, last_page]", callback)
        for field in (
            "daddr",
            "asid",
            "cpu_vaddr",
            "cached_count",
            "entry_valid",
            "raw",
            "type",
            "marked",
            "pointer_null",
            "is_current",
            "first_page",
            "last_page",
        ):
            self.assertIn(f"{field}=", block)
        self.assertNotIn("GetPageCacheInfo", callback)
        self.assertNotIn("GetPageEntryInfo", callback)

    def test_device_snapshot_reads_stored_backing_and_atomic_counter(self):
        source = (ROOT / "src/core/device_memory_manager.inc").read_text()
        block = source.split("::GetPageCacheInfo(DAddr address) const {", 1)[1].split(
            "template <typename Traits>", 1
        )[0]
        self.assertIn("address >= device_as_size", block)
        self.assertIn("ExtractCPUBacking(page)", block)
        self.assertIn("Count(page).load(std::memory_order_relaxed)", block)
        self.assertIn("asid.id < registered_processes.size()", block)
        self.assertIn("registered_processes[asid.id]", block)
        self.assertIn("PageCacheInfo{asid, cpu_vaddr, cached_count, memory}", block)
        for forbidden in ("fetch_add", "fetch_sub", "RasterizerMarkRegionCached", "CommitRegion"):
            self.assertNotIn(forbidden, block)

    def test_cpu_snapshot_uses_owner_and_one_raw_read_without_callbacks(self):
        source = (ROOT / "src/core/memory.cpp").read_text()
        block = source.split("Memory::GetPageEntryInfo(Common::ProcessAddress vaddr) const {", 1)[
            1
        ].split("void Memory::MapMemoryRegion", 1)[0]
        self.assertIn("impl->owning_process->GetPageTable().GetImpl()", block)
        self.assertIn("page >= page_table.entries.size()", block)
        self.assertEqual(block.count(".Raw()"), 2)  # bitmap view + raw storage view
        self.assertIn("static_cast<Common::PageType>(raw.type)", block)
        self.assertIn("raw.marked != 0", block)
        self.assertIn("PageEntryData::ExtractPointer(raw) == 0", block)
        self.assertIn("&page_table == impl->current_page_table", block)
        for forbidden in (
            "GetPointer",
            "HandleRasterizer",
            "CommitRegion",
            "GetUnchecked",
            ".Store(",
            "MarkRasterizerCached",
        ):
            self.assertNotIn(forbidden, block)


if __name__ == "__main__":
    unittest.main()
