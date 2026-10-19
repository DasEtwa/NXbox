// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include "dynarmic/interface/optimization_flags.h"

namespace Core::NxboxJit {
inline bool BisectEnabled(const char* name) noexcept {
    const auto* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

inline std::uint32_t UwpCodeCacheSize() noexcept {
    // DBSZ exhausts 128 MiB and recompiles hundreds of thousands of blocks after evacuation.
    // A larger working set can avoid those resets, but costs up to 256 MiB across four cores.
    // Keep the experiment opt-in: the captured process already peaks near its 5 GiB limit.
    static const bool large_cache = BisectEnabled("NXBOX_JIT_LARGE_CACHE");
    return (large_cache ? 192u : 128u) * 1024u * 1024u;
}

inline bool Disabled(const char* name) noexcept {
    const auto* value = std::getenv(name);
    return value && std::strcmp(value, "0") == 0;
}

// Apply after accuracy/debugger settings. An environment variable can only remove an
// optimization, never turn one back on against the user's accuracy selection.
template <typename Config>
void ApplyOptOuts(Config& config) {
    using Flag = Dynarmic::OptimizationFlag;
    struct Option {
        const char* name;
        Flag flag;
    };
    constexpr Option options[] = {
        {"NXBOX_JIT_BLOCK_LINKING", Flag::BlockLinking},
        {"NXBOX_JIT_RSB", Flag::ReturnStackBuffer},
        {"NXBOX_JIT_FAST_DISPATCH", Flag::FastDispatch},
        {"NXBOX_JIT_CONTEXT_ELIMINATION", Flag::GetSetElimination},
        {"NXBOX_JIT_CONST_PROP", Flag::ConstProp},
        {"NXBOX_JIT_MISC_IR", Flag::MiscIROpt},
        {"NXBOX_JIT_CODE_SPEED", Flag::CodeSpeed},
        {"NXBOX_JIT_DISABLE_VERIFICATION", Flag::DisableVerification},
        {"NXBOX_JIT_UNFUSE_FMA", Flag::Unsafe_UnfuseFMA},
        {"NXBOX_JIT_REDUCED_ERROR_FP", Flag::Unsafe_ReducedErrorFP},
        {"NXBOX_JIT_INACCURATE_NAN", Flag::Unsafe_InaccurateNaN},
        {"NXBOX_JIT_IGNORE_STANDARD_FPCR", Flag::Unsafe_IgnoreStandardFPCRValue},
        {"NXBOX_JIT_IGNORE_GLOBAL_MONITOR", Flag::Unsafe_IgnoreGlobalMonitor},
    };
    for (const auto& option : options) {
        if (Disabled(option.name)) {
            config.optimizations &= ~option.flag;
        }
    }
    if (Disabled("NXBOX_JIT_PAGE_TABLE")) {
        config.page_table = nullptr;
    }
    if (Disabled("NXBOX_JIT_FASTMEM")) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    if (Disabled("NXBOX_JIT_FASTMEM_EXCLUSIVES")) {
        config.fastmem_exclusive_access = false;
    }
    if (Disabled("NXBOX_JIT_RECOMPILE_EXCLUSIVES")) {
        config.recompile_on_exclusive_fastmem_failure = false;
    }
    if (Disabled("NXBOX_JIT_REDUCE_MISALIGNMENT_CHECKS")) {
        config.only_detect_misalignment_via_page_table_on_page_boundary = false;
    }
}
// A64-only diagnostics and Auto's unchecked host address-space optimization.
template <typename Config>
void ApplyA64OptOuts(Config& config, std::uint32_t address_space_bits) {
    ApplyOptOuts(config);
    config.collect_memory_access_context = !Disabled("NXBOX_JIT_PRECISE_MEMORY");
    if (Disabled("NXBOX_JIT_UNCHECKED_FASTMEM")) {
        config.fastmem_address_space_bits = address_space_bits;
    }
}
} // namespace Core::NxboxJit
