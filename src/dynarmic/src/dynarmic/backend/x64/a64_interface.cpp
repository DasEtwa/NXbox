// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <string_view>

#include "common/nxbox_jit_cache.h"

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include <bit>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#if __has_include("../../../../../common/nxbox_stall.h")
#    include "../../../../../common/nxbox_stall.h"
#endif
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

#if NXBOX_STALL_PROFILE
#    include <unordered_map>
#    include <unordered_set>
#endif

namespace Dynarmic::A64 {

using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, reinterpret_cast<u64>(arg)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::AddTicks>(cb)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

struct Jit::Impl final {
public:
    Impl(Jit* jit, UserConfig conf)
        : conf(conf)
        , block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf))
        , emitter(block_of_code, conf, jit)
        , polyfill_options(GenPolyfillOptions(block_of_code))
    {
        if (conf.disk_cache && !conf.very_verbose_debugging_output) {
            try {
                disk_config = DiskConfig();
                if (!conf.disk_cache->RegisterConfig(disk_config)) {
                    disk_config.clear();
                }
            } catch (...) {
                disk_config.clear();
            }
        }
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
    }

    ~Impl() = default;

    NxboxJitCache::Bytes DiskConfig() const {
        NxboxJitCache::Bytes key;
#ifdef NXBOX_JIT_EMITTER_ID
        constexpr std::string_view emitter_id{NXBOX_JIT_EMITTER_ID};
        key.insert(key.end(), emitter_id.begin(), emitter_id.end());
#else
        // Unversioned builds must never load executable cache entries.
        return key;
#endif
        const auto put = [&](auto value) { NxboxJitCache::Put(key, static_cast<u64>(value)); };
        put(NxboxJitCache::FormatVersion);
        put(sizeof(A64JitState));
        put(sizeof(StackLayout));
        put(ABI_SHADOW_SPACE);
        for (u32 bit = 1; bit <= static_cast<u32>(HostFeature::FastBMI2); bit <<= 1) {
            put(block_of_code.HasHostFeature(static_cast<HostFeature>(bit)));
        }
        put(conf.optimizations);
        put(conf.fastmem_pointer.has_value());
        put(conf.page_table != nullptr);
        put(conf.global_monitor != nullptr);
        put(conf.tpidr_el0 != nullptr);
        put(conf.tpidrro_el0 != nullptr);
        put(conf.page_table_address_space_bits);
        put(conf.page_table_pointer_mask);
        put(conf.page_table_log2_stride);
        put(conf.page_table_marked_bit ? *conf.page_table_marked_bit : 256);
        put(conf.page_table_sign_extension ? *conf.page_table_sign_extension : 256);
        put(conf.cntfrq_el0);
        put(conf.ctr_el0);
        put(conf.dczid_el0);
        put(conf.fastmem_address_space_bits);
        put(conf.code_cache_size);
        put(conf.detect_misaligned_access_via_page_table);
        put(conf.processor_id);
        put(conf.unsafe_optimizations);
        put(conf.hook_data_cache_operations);
        put(conf.hook_isb);
        put(conf.hook_hint_instructions);
        put(conf.silently_mirror_page_table);
        put(conf.absolute_offset_page_table);
        put(conf.only_detect_misalignment_via_page_table_on_page_boundary);
        put(conf.recompile_on_fastmem_failure);
        put(conf.silently_mirror_fastmem);
        put(conf.fastmem_exclusive_access);
        put(conf.recompile_on_exclusive_fastmem_failure);
        put(conf.define_unpredictable_behaviour);
        put(conf.wall_clock_cntpct);
        put(conf.check_halt_on_memory_access);
        put(conf.collect_memory_access_context);
        put(conf.enable_cycle_counting);
        put(conf.very_verbose_debugging_output);
        return key;
    }

    bool MatchesGuest(const NxboxJitCache::Record& record) {
        return NxboxJitCache::MatchesGuest(
            record, [this](u64 pc) { return conf.callbacks->MemoryReadCodeFresh(pc); });
    }

    static bool CheckDiskBlockThunk(void* context, const NxboxJitCache::Record* record) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (self.MatchesGuest(*record)) {
                return true;
            }
        } catch (...) {
        }
        // Exit before executing even one instruction from the stale cached body.
        self.jit_state.pc = record->pc;
#if NXBOX_STALL_PROFILE
        NxboxStall::AddJit(NxboxStall::JitEvent::DiskStale);
#endif
        self.InvalidateCacheRange(record->pc, record->guest.size());
        self.conf.disk_cache->Result(false);
        return false;
    }

    HaltReason Run() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        // TODO: Check code alignment
        const CodePtr current_code_ptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSB_PTR_MASK;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }
            return CodePtr((uintptr_t(GetCurrentBlock()) + 15) & ~uintptr_t(15));
        }();
        const HaltReason hr = block_of_code.RunCode(&jit_state, current_code_ptr);
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        const HaltReason hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    void ClearCache() {
#if NXBOX_STALL_PROFILE
        NxboxStall::AddJit(NxboxStall::JitEvent::ClearRequests);
#endif
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        // A zero-length range must not underflow into an invalidation of all guest code.
        if (length == 0) {
            return;
        }
#if NXBOX_STALL_PROFILE
        NxboxStall::AddJit(NxboxStall::JitEvent::RangeCalls);
        NxboxStall::AddJit(NxboxStall::JitEvent::RangeBytes, length);
#endif
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

private:
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        if (auto block = emitter.GetBasicBlock(descriptor))
            return block->entrypoint;

#if NXBOX_STALL_PROFILE
        const NxboxStall::Scope stall_scope{NxboxStall::Kind::Jit};
#endif
        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            // Immediately evacuate cache
#if NXBOX_STALL_PROFILE
            const NxboxStall::Scope flush_scope{NxboxStall::Kind::JitFlush};
#endif
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation, true);
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        if (conf.disk_cache && !disk_config.empty() &&
            !A64::LocationDescriptor{descriptor}.SingleStepping()) {
            if (auto saved = conf.disk_cache->Find(descriptor.Value(), disk_config)) {
                try {
                    if (MatchesGuest(*saved)) {
                        const auto it =
                            restored_records.insert_or_assign(descriptor.Value(), std::move(*saved))
                                .first;
                        if (auto restored =
                                emitter.Restore(it->second, CheckDiskBlockThunk, this)) {
                            // Guards need only the guest footprint; free duplicate host/config
                            // bytes.
                            NxboxJitCache::Bytes{}.swap(it->second.host);
                            NxboxJitCache::Bytes{}.swap(it->second.config);
                            conf.disk_cache->Result(true, restored->size);
                            return restored->entrypoint;
                        }
                    }
                    conf.disk_cache->Result(false);
                } catch (...) {
                    conf.disk_cache->Result(false);
                }
            }
        }
        // Remember the exact instruction reads used by the translator, not a later snapshot.
        std::map<u64, u32> instruction_reads;
        bool record_reads = conf.disk_cache && !disk_config.empty();
        u64 guest_code_reads = 0;
        const auto get_code = [this, &instruction_reads, &record_reads, &guest_code_reads](u64 vaddr) {
            ++guest_code_reads;
            const auto word = conf.disk_cache ? conf.callbacks->MemoryReadCodeFresh(vaddr)
                                              : conf.callbacks->MemoryReadCode(vaddr);
            if (record_reads && word) {
                try {
                    const auto [it, inserted] = instruction_reads.emplace(vaddr, *word);
                    if (!inserted && it->second != *word) {
                        // Translation observed changing code; never persist an ambiguous body.
                        record_reads = false;
                        instruction_reads.clear();
                    }
                } catch (...) {
                    record_reads = false;
                    instruction_reads.clear();
                }
            }
            return word;
        };
        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        ir_block.Reset(arch_descriptor);
        {
#if NXBOX_STALL_PROFILE
            const NxboxStall::Scope scope{NxboxStall::Kind::JitTranslate};
#endif
            A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
        }
#if NXBOX_STALL_PROFILE
        NxboxStall::AddJit(NxboxStall::JitEvent::GuestCodeReads, guest_code_reads);
#endif
        bool disk_eligible = record_reads && emitter.DiskCacheEligible(ir_block);
        {
#if NXBOX_STALL_PROFILE
            const NxboxStall::Scope scope{NxboxStall::Kind::JitOptimize};
#endif
            Optimization::Optimize(ir_block, conf, polyfill_options);
        }
        NxboxJitCache::Record capture;
        if (disk_eligible) {
            capture.descriptor = descriptor.Value();
            capture.pc = arch_descriptor.PC();
            try {
                capture.config = disk_config;
            } catch (...) {
                disk_eligible = false;
            }
        }
        const auto block = [&] {
#if NXBOX_STALL_PROFILE
            const NxboxStall::Scope scope{NxboxStall::Kind::JitEmit};
#endif
            return emitter.Emit(ir_block, disk_eligible ? &capture : nullptr);
        }();
        if (disk_eligible && !capture.host.empty()) {
            try {
                const u64 end = A64::LocationDescriptor{ir_block.EndLocation()}.PC();
                const u64 size = end - capture.pc;
                bool complete = end > capture.pc && size <= NxboxJitCache::GuestLimit &&
                                size % 4 == 0 && instruction_reads.size() == size / 4;
                for (u64 offset = 0; complete && offset < size; offset += 4) {
                    const auto it = instruction_reads.find(capture.pc + offset);
                    if (it == instruction_reads.end()) {
                        complete = false;
                        break;
                    }
                    for (unsigned i = 0; i < 4; ++i) {
                        capture.guest.push_back(static_cast<u8>(it->second >> (i * 8)));
                    }
                }
                if (complete && MatchesGuest(capture)) {
                    conf.disk_cache->Insert(std::move(capture));
                }
            } catch (...) {
                // Recording failure cannot prevent normal translation/execution.
            }
        }
#if NXBOX_STALL_PROFILE
        // Per-JIT history survives invalidation. A key repeated here really was compiled again;
        // migration to another CPU and FPCR/single-step variants are not conflated with that.
        RecordPc(arch_descriptor.PC());
        RecordCompilation(seen_keys, descriptor.Value(), NxboxStall::JitEvent::NewKey,
                          NxboxStall::JitEvent::RepeatKey, NxboxStall::JitEvent::UnknownKey);
        // Sample across the entire descriptor space, rather than only the first 65k
        // startup keys. Counters are exact for sampled keys, not extrapolated estimates.
        RecordSampledCompilation(descriptor.Value());
        SampleCache(true);
#endif
        return block.entrypoint;
    }

    void PerformRequestedCacheInvalidation(HaltReason hr, [[maybe_unused]] bool capacity_clear = false) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

#if NXBOX_STALL_PROFILE
            const NxboxStall::Scope scope{NxboxStall::Kind::JitInvalidate};
#endif
            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
#if NXBOX_STALL_PROFILE
                NxboxStall::AddJit(NxboxStall::JitEvent::CacheClears);
                NxboxStall::AddJit(capacity_clear ? NxboxStall::JitEvent::CapacityClears
                                                  : NxboxStall::JitEvent::ExplicitClears);
#endif
                block_of_code.ClearCache();
                emitter.ClearCache();
                restored_records.clear();
            } else {
#if NXBOX_STALL_PROFILE
                NxboxStall::AddJit(NxboxStall::JitEvent::RangeBatches);
#endif
                emitter.InvalidateCacheRanges(invalid_cache_ranges);
            }
#if NXBOX_STALL_PROFILE
            SampleCache(false, invalidate_entire_cache);
#endif
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

#if NXBOX_STALL_PROFILE
    void SampleCache(bool compilation = false, bool cleared = false) {
        NxboxStall::SampleJitCache(conf.processor_id,
                                   conf.code_cache_size - block_of_code.SpaceRemaining(), conf.code_cache_size, compilation, cleared);
    }

    static constexpr size_t HISTORY_LIMIT = 65536;

    void RecordPc(u64 pc) {
        auto it = seen_pcs.find(pc);
        if (it != seen_pcs.end()) {
            ++it->second;
            NxboxStall::AddJit(NxboxStall::JitEvent::RepeatPc);
        } else if (seen_pcs.size() < HISTORY_LIMIT) {
            it = seen_pcs.emplace(pc, 1).first;
            NxboxStall::AddJit(NxboxStall::JitEvent::NewPc);
        } else {
            NxboxStall::AddJit(NxboxStall::JitEvent::UnknownPc);
            return;
        }
        NxboxStall::MaxJit(NxboxStall::JitEvent::PcCompileMax, it->second);
    }

    static void RecordCompilation(std::unordered_set<u64>& history, u64 value, NxboxStall::JitEvent first, NxboxStall::JitEvent repeat, NxboxStall::JitEvent unknown) {
        // Bound profiling memory. Untracked values are explicitly unknown, never called new.
        if (history.find(value) != history.end()) {
            NxboxStall::AddJit(repeat);
        } else if (history.size() < HISTORY_LIMIT) {
            history.insert(value);
            NxboxStall::AddJit(first);
        } else {
            NxboxStall::AddJit(unknown);
        }
    }

    void RecordSampledCompilation(u64 key) {
        // SplitMix64 finalizer: aligned PCs and FPCR variants all participate.
        u64 hash = key;
        hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
        hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
        hash ^= hash >> 31;
        if ((hash & 63) == 0) {
            RecordCompilation(sampled_keys, key, NxboxStall::JitEvent::SampledNewKey,
                              NxboxStall::JitEvent::SampledRepeatKey,
                              NxboxStall::JitEvent::SampledUnknownKey);
        }
    }

    std::unordered_set<u64> sampled_keys;
    std::unordered_map<u64, u64> seen_pcs;
    std::unordered_set<u64> seen_keys;
#endif

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    NxboxJitCache::Bytes disk_config;
    std::map<u64, NxboxJitCache::Record> restored_records;
    bool is_executing = false;
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
