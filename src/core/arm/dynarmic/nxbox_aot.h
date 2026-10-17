// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/arm/aot/precompile.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process.h"

namespace Core::Aot {
// Snapshot final executable mappings only after the loader completes. A 64 MiB cap
// bounds transient memory per core. This does not treat the entire ASLR region as code.
template <typename Jit, typename Report>
void WarmMappedCode(Kernel::KProcess& process, Jit& jit, Report&& report,
                    std::span<const Address> module_roots = {}) {
    const auto start = std::chrono::steady_clock::now();
    Module module;
    module.roots.assign(module_roots.begin(), module_roots.end());
    module.roots.push_back(GetInteger(process.GetEntryPoint()));
    const auto& table = process.GetPageTable();
    auto address = GetInteger(table.GetCodeRegionStart());
    const auto region_size = table.GetCodeRegionSize();
    if (region_size > std::numeric_limits<Address>::max() - address) {
        return;
    }
    const auto end = address + region_size;
    constexpr std::size_t snapshot_limit = 64 * 1024 * 1024;
    std::size_t bytes{}, regions{};
    bool truncated{};
    while (address < end && regions++ < 4096) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        if (table.QueryInfo(&info, &page, address).IsError() || !info.GetSize() ||
            info.GetSize() > std::numeric_limits<Address>::max() - info.GetAddress()) {
            truncated = true;
            break;
        }
        const auto next = info.GetEndAddress();
        if (next <= address) {
            truncated = true;
            break;
        }
        const auto permission = info.GetPermission();
        const bool executable = (permission & Kernel::KMemoryPermission::UserExecute) !=
                                Kernel::KMemoryPermission::None;
        const bool readonly =
            (permission & Kernel::KMemoryPermission::UserRead) != Kernel::KMemoryPermission::None &&
            (permission & Kernel::KMemoryPermission::UserWrite) == Kernel::KMemoryPermission::None;
        if (executable || readonly) {
            const auto size = std::min<Address>(next, end) - address;
            if (size > snapshot_limit - bytes) {
                truncated = true;
                break;
            }
            Range range{address, std::vector<std::uint8_t>(size), executable};
            process.GetMemory().ReadBlock(address, range.bytes.data(), range.bytes.size());
            module.ranges.push_back(std::move(range));
            bytes += size;
        }
        address = next;
    }
    auto discovery = Discover(module);
    discovery.truncated |= truncated || address < end;
    auto stats = Precompile(discovery, [&](Address pc) { return CompileBlock(jit, pc); });
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    report(stats, discovery, bytes, elapsed);
}
} // namespace Core::Aot
