// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include "core/arm/aot/discovery.h"

namespace Core::Aot {
inline bool Enabled() {
    const auto* value = std::getenv("NXBOX_AOT");
    return value && std::strcmp(value, "1") == 0;
}
enum class CompileResult { Restored, Compiled, Unsupported };
struct Statistics {
    std::size_t discovered{}, restored{}, compiled{}, unsupported{};
    std::size_t runtime_compiled{}, runtime_undiscovered{}, invalidated{};
    std::uint64_t boot_microseconds{};
    // Only call from the translator's compile event, never instruction-read callbacks.
    void RuntimeCompile(Address pc, const Discovery& discovery) {
        ++runtime_compiled;
        runtime_undiscovered += !discovery.blocks.contains(pc);
    }
};
// Backend must compile/restore without executing instructions or changing architectural
// state, using its actual FPCR/location descriptor/config and existing persistent store.
// Each core owns its host code and must warm separately. No invented x86 bytes here.
template <typename Compile>
Statistics Precompile(const Discovery& discovery, Compile&& compile) {
    const auto start = std::chrono::steady_clock::now();
    Statistics stats;
    stats.discovered = discovery.blocks.size();
    for (auto pc : discovery.blocks) {
        switch (compile(pc)) {
        case CompileResult::Restored:
            ++stats.restored;
            break;
        case CompileResult::Compiled:
            ++stats.compiled;
            break;
        case CompileResult::Unsupported:
            ++stats.unsupported;
            break;
        }
    }
    stats.boot_microseconds =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
    return stats;
}
// Enables integration once T19 supplies this API. An absent hook fails closed rather
// than using Run/Step to warm (which would execute SVCs and mutate the guest).
template <typename Jit>
CompileResult CompileBlock(Jit& jit, Address pc) {
    if constexpr (requires { jit.PrecompileBlock(pc); }) {
        return static_cast<CompileResult>(jit.PrecompileBlock(pc));
    } else {
        return CompileResult::Unsupported;
    }
}
} // namespace Core::Aot
