// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#    pragma warning(push, 0)
#    include <unicorn/unicorn.h>
#    pragma warning(pop)
#else
#    include <unicorn/unicorn.h>
#endif

#include "common/common_types.h"

#include "dynarmic/tests/A32/testenv.h"

namespace Unicorn::A32 {
static constexpr size_t num_gprs = 16;
static constexpr size_t num_ext_regs = 64;

using ExtRegArray = std::array<u32, num_ext_regs>;
using RegisterArray = std::array<u32, num_gprs>;
using RegisterPtrArray = std::array<RegisterArray::pointer, num_gprs>;
using RegisterConstPtrArray = std::array<RegisterArray::const_pointer, num_gprs>;
}  // namespace Unicorn::A32

template<class TestEnvironment>
class A32Unicorn final {
public:
    using ExtRegArray = Unicorn::A32::ExtRegArray;
    using RegisterArray = Unicorn::A32::RegisterArray;

    explicit A32Unicorn(TestEnvironment& testenv);
    ~A32Unicorn();

    void Run(u32 end_address = 0);

    u8 ReadMemoryByte(u32 address) const;

    u32 GetSP() const;
    void SetSP(u32 value);

    u32 GetPC() const;
    void SetPC(u32 value);

    RegisterArray GetRegisters() const;
    void SetRegisters(const RegisterArray& value);

    ExtRegArray GetExtRegs() const;
    void SetExtRegs(const ExtRegArray& value);

    u32 GetFpscr() const;
    void SetFpscr(u32 value);

    u32 GetFpexc() const;
    void SetFpexc(u32 value);

    u32 GetCpsr() const;
    void SetCpsr(u32 value);

    void EnableFloatingPointAccess();

    void ClearPageCache();

    void DumpMemoryInformation();

private:
    void CommitWrites();
    void CompleteInstruction(u32 next_pc);
    static void CodeHook(uc_engine* uc, u64 address, u32 size, void* user_data);
    static void InterruptHook(uc_engine* uc, u32 interrupt, void* user_data);
    static bool UnmappedMemoryHook(uc_engine* uc, uc_mem_type type, u32 addr, int size, u64 value, void* user_data);
    static bool MemoryWriteHook(uc_engine* uc, uc_mem_type type, u32 addr, int size, u64 value, void* user_data);

    struct Page {
        u32 address;
        std::array<u8, 4096> data;
    };

    TestEnvironment& testenv;
    uc_engine* uc{};
    uc_hook code_hook{};
    uc_hook intr_hook{};
    uc_hook mem_invalid_hook{};
    uc_hook mem_write_prot_hook{};

    std::vector<std::unique_ptr<Page>> pages;
    std::vector<std::pair<u32, int>> pending_writes;
    u32 previous_pc = 0;
    u32 previous_cpsr = 0;
    bool have_previous_instruction = false;
};
