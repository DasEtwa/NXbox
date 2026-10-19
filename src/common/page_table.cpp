// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <mutex>
#include <string_view>

#include "common/page_table.h"
#if defined(_WIN32) && defined(YUZU_UWP_APPCONTAINER)
#include "common/sparse_memory.h"
#endif
#include "common/scope_exit.h"

namespace Common {

bool PageTable::CleanJitPageTableEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_CPU_CLEAN_PAGE_TABLE");
        return !value || std::string_view{value} != "0";
    }();
    return enabled;
}

void PageTable::LogJitPageTable() {
    static std::once_flag flag;
    std::call_once(flag, [this] {
        LOG_INFO(Core_ARM,
                 "NXBOX_CPU_CLEAN_PAGE_TABLE={} active={} fastmem={} "
                 "(latched at first page table)",
                 CleanJitPageTableEnabled(), UseCleanJitPageTable(), fastmem_arena != nullptr);
    });
}

void PageTable::LogJitPageTableMemoryOnce() {
    if (jit_memory_logged.exchange(true, std::memory_order_relaxed)) {
        return;
    }
#if defined(_WIN32) && defined(YUZU_UWP_APPCONTAINER)
    LOG_INFO(
        Core_ARM,
        "NXBOX clean page table after boot: reserved_bytes={} "
        "committed_bytes={} bitmap_bytes={}",
        jit_entries.size() * sizeof(JitPageEntry),
        SparseMemory::CommittedBytes(jit_entries.data(), jit_entries.size() * sizeof(JitPageEntry)),
        ((jit_entries.size() * sizeof(JitPageEntry) + HostPageSize * 64 - 1) /
         (HostPageSize * 64)) *
            sizeof(u64));
#endif
}

PageTable::PageTable() = default;

PageTable::~PageTable() noexcept = default;

void PageTable::Resize(std::size_t address_space_width_in_bits, std::size_t page_bits) {
    auto const num_page_table_entries = 1ULL << (address_space_width_in_bits - page_bits);
    // Resize/reset is only legal with readers stopped. ResizeAndClear alone does not
    // clear an existing reservation when its size stays the same.
    if (entries.size() == num_page_table_entries) {
        if (jit_entries.size()) {
            jit_entries.ZeroRegion(0, jit_entries.size());
        }
        entries.ZeroRegion(0, entries.size());
    }
    entries.ResizeAndClear(num_page_table_entries);
    if (CleanJitPageTableEnabled()) {
        jit_entries.ResizeAndClear(num_page_table_entries);
    }
    jit_memory_logged.store(false, std::memory_order_relaxed);
    current_address_space_width_in_bits = address_space_width_in_bits;
    current_page_bits = page_bits;
}

} // namespace Common
