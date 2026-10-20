// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <string_view>

#include "common/logging.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/service.h"

namespace Service {

namespace AmFsPolling {

// Opt-in attribution only; never changes guest results, events or timing policies.
// Capture with NXBOX_AM_FS_POLL_TRACE=1, NXBOX_WAIT_DIAG=1 and
// NXBOX_AM_FS_POLL_THREAD=82 (the DBSZ loading thread; 0 selects all threads).
// Unlike NXBOX_SERVICE_TRACE, repeated command/result pairs are retained. Sequence
// numbers pair begin/end records even when service requests overlap. No guest
// buffers, filenames or IPC payloads are logged. A visible limit bounds log volume.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_AM_FS_POLL_TRACE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}

inline u64 ThreadFilter() {
    static const u64 filter = [] {
        const char* value = std::getenv("NXBOX_AM_FS_POLL_THREAD");
        u64 thread = 82;
        if (value) {
            const std::string_view text{value};
            u64 parsed{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) {
                thread = parsed;
            }
        }
        return thread;
    }();
    return filter;
}

inline constexpr u64 RequestLimit = 65536;
inline std::atomic<u64> sequence{};

} // namespace AmFsPolling

// Scoped to AM and FS so investigation does not require changing shared IPC code.
template <typename Self>
class PollingServiceFramework : public ServiceFramework<Self> {
public:
    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        if (!AmFsPolling::Enabled()) {
            return ServiceFramework<Self>::HandleSyncRequest(session, ctx);
        }
        const u64 thread = ctx.GetThread().GetThreadId();
        const u64 filter = AmFsPolling::ThreadFilter();
        if (filter != 0 && thread != filter) {
            return ServiceFramework<Self>::HandleSyncRequest(session, ctx);
        }
        const u64 sequence = AmFsPolling::sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sequence > AmFsPolling::RequestLimit) {
            if (sequence == AmFsPolling::RequestLimit + 1) {
                LOG_WARNING(Service, "AM_FS_POLL truncated limit={} thread_filter={}",
                            AmFsPolling::RequestLimit, filter);
            }
            return ServiceFramework<Self>::HandleSyncRequest(session, ctx);
        }
        const u32 command = ctx.GetCommand();
        const auto type = static_cast<u32>(ctx.GetCommandType());
        LOG_WARNING(
            Service, "AM_FS_POLL begin seq={} thread={} service={} type={} cmd={} interface={}",
            sequence, thread, this->GetServiceName(), type, command, ServiceInterfaceName<Self>());
        const auto started = std::chrono::steady_clock::now();
        const Result transport = ServiceFramework<Self>::HandleSyncRequest(session, ctx);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const bool deferred = ctx.GetIsDeferred();
        const bool has_result = ctx.HasResponse() && !deferred;
        const u32 result = has_result ? ctx.CommandBuffer()[ctx.GetDataPayloadOffset()] : 0;
        LOG_WARNING(
            Service,
            "AM_FS_POLL end seq={} thread={} service={} type={} cmd={} interface={} elapsed_us={} "
            "has_result={} result={:#010x} deferred={} transport={:#010x}",
            sequence, thread, this->GetServiceName(), type, command, ServiceInterfaceName<Self>(),
            elapsed, has_result, result, deferred, transport.raw);
        return transport;
    }

protected:
    explicit PollingServiceFramework(Core::System& system, const char* name,
                                     u32 max_sessions = ServerSessionCountMax)
        : ServiceFramework<Self>{system, name, max_sessions} {}
};

} // namespace Service
