# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production sparse page tables under memory and thread sanitizers."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CleanJitPageTableTests(unittest.TestCase):
    def test_native_sequences_and_ordering(self):
        # fmt is already a project dependency; ask pkg-config rather than fetching it.
        flags = subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "fmt"], text=True
        ).split()
        with tempfile.TemporaryDirectory() as directory:
            for sanitizer in ("address,undefined", "thread"):
                binary = Path(directory) / sanitizer.replace(",", "-")
                command = [
                    "clang++",
                    "-std=c++20",
                    "-g",
                    "-pthread",
                    f"-fsanitize={sanitizer}",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/clean_jit_page_table_test.cpp"),
                    str(ROOT / "src/common/page_table.cpp"),
                    str(ROOT / "src/common/sparse_large_vector.cpp"),
                    *flags,
                    "-o",
                    str(binary),
                ]
                subprocess.run(command, check=True, capture_output=True, text=True)
                for enabled in ("1", "0"):
                    env = dict(os.environ, NXBOX_CPU_CLEAN_PAGE_TABLE=enabled)
                    args = [str(binary)] + ([] if enabled == "1" else ["disabled"])
                    result = subprocess.run(
                        args,
                        env=env,
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=120,
                    )
                    print(f"{sanitizer}: {result.stdout.strip()}")

    def test_allocation_waits_for_fastmem_selection(self):
        page_table = (ROOT / "src/common/page_table.cpp").read_text()
        resize = page_table.split("void PageTable::Resize(", 1)[1]
        self.assertNotIn("jit_entries.ResizeAndClear", resize)
        memory = (ROOT / "src/core/memory.cpp").read_text()
        selection = memory.split("void SetCurrentPageTable(", 1)[1].split("#ifdef __ANDROID__", 1)[
            0
        ]
        self.assertGreater(
            selection.index("current_page_table->InitializeJitPageTable();"),
            selection.index("current_page_table->fastmem_arena = nullptr;"),
        )

    def test_memory_log_captures_shutdown_before_teardown(self):
        core = (ROOT / "src/core/core.cpp").read_text()
        run = core.split("    void Run() {", 1)[1].split("    void Pause()", 1)[0]
        self.assertNotIn("LogJitPageTableMemoryOnce", run)
        shutdown = core.split("    void ShutdownMainProcess() {", 1)[1].split(
            "    bool IsShuttingDown()", 1
        )[0]
        log = shutdown.index("LogJitPageTableMemoryOnce();")
        self.assertLess(shutdown.index("kernel.SuspendEmulation(true);"), log)
        self.assertLess(log, shutdown.index("kernel.CloseServices();"))
        self.assertLess(log, shutdown.index("kernel.ShutdownCores();"))
        self.assertLess(log, shutdown.index("services.reset();"))
        self.assertLess(log, shutdown.index("kernel.Shutdown();"))
        self.assertEqual(core.count("LogJitPageTableMemoryOnce();"), 1)
        page_table = (ROOT / "src/common/page_table.cpp").read_text()
        self.assertIn("NXBOX clean page table at shutdown:", page_table)

    def test_jit_configuration_and_writer_wiring(self):
        for bits in (32, 64):
            source = (ROOT / f"src/core/arm/dynarmic/arm_dynarmic_{bits}.cpp").read_text()
            block = source.split("if (page_table->UseCleanJitPageTable()) {", 1)[1].split(
                "\n        }", 1
            )[0]
            self.assertIn("page_table->jit_entries.data()", block)
            self.assertIn("config.page_table_pointer_mask = 0;", block)
            self.assertIn("config.page_table_marked_bit = std::nullopt;", block)
            self.assertIn("config.page_table_sign_extension = std::nullopt;", block)
            self.assertIn(
                "config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;", source
            )
            self.assertIn("config.page_table_marked_bit = uint8_t(0);", source)
            self.assertIn("config.absolute_offset_page_table = true;", source)
            self.assertIn("config.page_table = nullptr;", source)
        memory = (ROOT / "src/core/memory.cpp").read_text()
        self.assertIn(
            "ClearJitRegion(base, end);\n            page_table.entries.ZeroRegion", memory
        )
        for mark in ("MarkDebug(pointer, block)", "MarkRasterizerCached()"):
            prefix = memory.split(f"entry.{mark};", 1)[0]
            self.assertTrue(prefix.rstrip().endswith("ClearJitPage(vaddr >> YUZU_PAGEBITS);"))
        self.assertNotIn("entry.Store(", memory)
        self.assertIn("page_table.StorePage(base, false, type, current_block, host_ptr);", memory)


if __name__ == "__main__":
    unittest.main()
