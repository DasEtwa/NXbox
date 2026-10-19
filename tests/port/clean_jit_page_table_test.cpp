// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "common/page_table.h"

// Keep the real table/allocation code; only the application logger/assert sink
// is stubbed.
void AssertFailSoftImpl() { std::abort(); }
[[noreturn]] void AssertFatalImpl() { std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char *, unsigned int, const char *,
                       fmt::string_view, const fmt::format_args &) {}
} // namespace Common::Log

int main(int argc, char **) {
  using Common::PageTable;
  using Common::PageType;
  const bool enabled = argc == 1;
  assert(PageTable::CleanJitPageTableEnabled() == enabled);
  // The environment is latched for the process, even if changed after first
  // use.
  setenv("NXBOX_CPU_CLEAN_PAGE_TABLE", enabled ? "0" : "1", 1);
  assert(PageTable::CleanJitPageTableEnabled() == enabled);
  PageTable table;
  table.Resize(39, 12);
  assert(table.entries.size() == (1ULL << 27));
  assert(table.jit_entries.size() == (enabled ? 1ULL << 27 : 0));
  assert(table.UseCleanJitPageTable() == enabled);
  table.fastmem_arena = reinterpret_cast<u8 *>(0x1000);
  assert(!table.UseCleanJitPageTable());
  table.fastmem_arena = nullptr;
  constexpr std::size_t page = 31;
  constexpr uintptr_t pointer = 0xfffffff000000000ULL;
  auto check = [&](uintptr_t expected) {
    if (enabled) {
      assert(table.jit_entries[page].Load() == expected);
    }
  };
  check(0);
  table.StorePage(page, false, PageType::Memory, 40000, pointer);
  assert(table.entries[page].Pointer() == pointer);
  assert(table.entries[page].Block() == 40000);
  check(pointer);
  table.ClearJitPage(page);
  table.entries.GetUnchecked(page).MarkDebug(pointer, 40000);
  check(0);
  table.StorePage(page, false, PageType::Memory, 40000, pointer);
  check(pointer);
  table.ClearJitPage(page);
  table.entries.GetUnchecked(page).MarkRasterizerCached();
  check(0);
  assert(table.entries[page].Type() == PageType::RasterizerCachedMemory);
  table.StorePage(page, false, PageType::Memory, 40000, pointer);
  check(pointer);
  table.StorePage(page, true, PageType::Memory, 40000, pointer);
  check(0);
  table.StorePage(page, false, PageType::Unmapped, 0, 0);
  check(0);
  if (enabled) {
    // Backing remains alive while a reader may have fetched its old
    // translation. A revoke cannot retract an already fetched pointer. After
    // observing the writer's completion, a new load must see zero. Handshake
    // prevents republish.
    alignas(4096) std::array<u8, 4096> backing{};
    backing[0] = 73;
    const auto race_pointer =
        reinterpret_cast<uintptr_t>(backing.data()) - (page << 12);
    std::atomic<unsigned> phase{0};
    std::atomic<unsigned> acknowledged{0};
    std::atomic<bool> stop{false};
    std::thread reader([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const unsigned before = phase.load(std::memory_order_acquire);
        const auto value = table.jit_entries[page].Load();
        assert(value == 0 || value == race_pointer);
        if (value) {
          assert(*reinterpret_cast<const u8 *>(value + (page << 12)) == 73);
        }
        if (before % 4 == 1) {
          assert(value == race_pointer);
          const auto [canonical_pointer, type, block] =
              table.entries[page].PointerTypeBlock();
          assert(canonical_pointer == race_pointer &&
                 type == PageType::Memory && block == 7);
        } else if (before % 4 == 3) {
          assert(value == 0);
        }
        acknowledged.store(before, std::memory_order_release);
      }
    });
    for (unsigned i = 1; i <= 20000; ++i) {
      table.StorePage(page, false, PageType::Memory, 7, race_pointer);
      phase.store(4 * i - 3, std::memory_order_release);
      while (acknowledged.load(std::memory_order_acquire) != 4 * i - 3) {
        std::this_thread::yield();
      }
      phase.store(4 * i - 2, std::memory_order_release);
      while (acknowledged.load(std::memory_order_acquire) != 4 * i - 2) {
        std::this_thread::yield();
      }
      table.ClearJitPage(page);
      if (i % 2) {
        table.entries.GetUnchecked(page).MarkRasterizerCached();
      } else {
        // The production unmap invalidates JIT slots before canonical
        // ZeroRegion.
        table.entries.ZeroRegion(page, page + 1);
      }
      phase.store(4 * i - 1, std::memory_order_release);
      while (acknowledged.load(std::memory_order_acquire) != 4 * i - 1) {
        std::this_thread::yield();
      }
      phase.store(4 * i, std::memory_order_release);
      // Let the reader finish its odd-phase load before publishing again.
      // A second acknowledgement is needed for a reader that already sampled
      // odd.
      while (acknowledged.load(std::memory_order_acquire) != 4 * i) {
        std::this_thread::yield();
      }
    }
    stop.store(true, std::memory_order_release);
    reader.join();
  }
  table.StorePage(page, false, PageType::Memory, 7, pointer);
  table.Resize(39, 12);
  assert(table.entries[page].Type() == PageType::Unmapped);
  check(0);
  PageTable other;
  other.Resize(32, 12);
  assert(other.entries[page].Type() == PageType::Unmapped);
  if (enabled) {
    assert(other.jit_entries[page].Load() == 0);
    other.StorePage(page, false, PageType::Memory, 1, pointer + 4096);
    assert(table.jit_entries[page].Load() == 0);
  }
  table.ClearJitRegion(0, 128);
  std::cout << "clean_table=" << enabled
            << " sequence/reset/process/fastmem/latch/revoke checks passed\n";
}
