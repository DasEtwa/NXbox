// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef NXBOX_UWP
#include <array>
#include <cstddef>
#include "common/common_types.h"

namespace Kernel {
class KThread;
}
namespace Core {
class ArmInterface;
namespace NxboxFault {

struct SvcEntry {
    u32 number{};
    u64 input_x0{};
    u64 output_x0{};
    u64 output_x1{};
    bool completed{};
    u64 entered_ms{};
    u64 finished_ms{};
    u64 pc{};
    u64 lr{};
};

// Owned by the guest thread, so history survives migration between physical cores.
struct SvcHistory {
    // Retain over a minute of 500 ms fatal-handler sleeps, including their caller.
    std::array<SvcEntry, 128> entries{};
    u64 count{};
};

struct Access {
    ArmInterface* arm{};
    Kernel::KThread* thread{};
    size_t core{};
    u64 address{};
    size_t size{};
    const char* operation{};
};

// Only populated inside a data-memory callback, never during translation or an SVC.
inline thread_local Access current_access{};
class AccessScope {
public:
    explicit AccessScope(Access access) : previous{current_access} {
        current_access = access;
    }
    ~AccessScope() {
        current_access = previous;
    }
    AccessScope(const AccessScope&) = delete;
    AccessScope& operator=(const AccessScope&) = delete;

private:
    Access previous;
};

} // namespace NxboxFault
} // namespace Core
#endif
