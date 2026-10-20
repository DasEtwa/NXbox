// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

// Opt-in flight recorder. Read by the frontend heartbeat, never by a GPU-thread task:
// diagnostics must still be available when guest work and presentation both stop.
namespace NxboxWait {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_WAIT_DIAG");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}
inline std::uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
enum class Counter {
    DecodeQueued,
    DecodeDispatched,
    DecodeStarted,
    DecodeCompleted,
    DecodeUploaded,
    DecodeCancelled,
    DecodeTicks,
    SealedTick,
    SubmittedTick,
    CompletedObservedTick,
    SyncpointQueued,
    SyncpointSignaled,
    QueryQueued,
    QueryWritten,
    QueryInvalidated,
    NvArmed,
    NvSignaled,
    NvCancelled,
    Count,
};
inline constexpr std::array CounterNames{
    "decode_queued",     "decode_dispatched",
    "decode_started",    "decode_completed",
    "decode_uploaded",   "decode_cancelled",
    "decode_ticks",      "sealed_tick",
    "submitted_tick",    "completed_observed_tick",
    "syncpoint_queued",  "syncpoint_signaled",
    "query_queued",      "query_written",
    "query_invalidated", "nv_armed",
    "nv_signaled",       "nv_cancelled",
};
static_assert(CounterNames.size() == static_cast<size_t>(Counter::Count));
inline std::array<std::atomic<std::uint64_t>, static_cast<size_t>(Counter::Count)> counters{};
inline void Add(Counter counter) {
    if (Enabled())
        counters[static_cast<size_t>(counter)].fetch_add(1, std::memory_order_relaxed);
}
inline void Max(Counter counter, std::uint64_t value) {
    if (!Enabled())
        return;
    auto& target = counters[static_cast<size_t>(counter)];
    auto old = target.load(std::memory_order_relaxed);
    while (old < value && !target.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
    }
}
enum class Kind { Svc, Service, Fence, NvEvent, Syncpoint };
inline constexpr std::array KindNames{"svc", "service", "fence", "nv_event", "syncpoint"};
struct Entry {
    Kind kind{};
    std::uint64_t key{}, token{}, began_ms{}, ended_ms{}, pc{}, lr{}, result{};
    std::array<std::uint64_t, 8> args{};
    std::array<char, 64> label{};
};
// Keep the last call per guest thread or NV event, including completed calls. No guest
// pointers are retained, no guest/kernel/GPU locks are acquired by the reader.
class Recorder {
public:
    std::uint64_t Begin(Kind kind, std::uint64_t key, std::string_view label,
                        std::span<const std::uint64_t> args = {}, std::uint64_t pc = 0,
                        std::uint64_t lr = 0) {
        std::scoped_lock lock{mutex};
        Entry* slot{};
        for (auto& entry : entries) {
            if (entry.token && entry.kind == kind && entry.key == key && entry.ended_ms) {
                slot = &entry;
                break;
            }
            if (!entry.token && !slot)
                slot = &entry;
        }
        if (!slot) {
            ++dropped;
            return 0;
        }
        *slot = {};
        slot->kind = kind;
        slot->key = key;
        slot->token = ++sequence;
        slot->began_ms = NowMs();
        slot->pc = pc;
        slot->lr = lr;
        std::copy_n(args.begin(), std::min(args.size(), slot->args.size()), slot->args.begin());
        std::copy_n(label.begin(), std::min(label.size(), slot->label.size() - 1),
                    slot->label.begin());
        return slot->token;
    }
    void End(std::uint64_t token, std::uint64_t result = 0) {
        if (!token)
            return;
        std::scoped_lock lock{mutex};
        for (auto& entry : entries) {
            if (entry.token == token && !entry.ended_ms) {
                entry.result = result;
                entry.ended_ms = NowMs();
                break;
            }
        }
    }
    void EndKey(Kind kind, std::uint64_t key, std::uint64_t result) {
        std::scoped_lock lock{mutex};
        for (auto& entry : entries) {
            if (entry.token && entry.kind == kind && entry.key == key && !entry.ended_ms) {
                entry.result = result;
                entry.ended_ms = NowMs();
            }
        }
    }
    struct Snapshot {
        std::vector<Entry> entries;
        std::uint64_t dropped{};
    };
    Snapshot Read() {
        std::scoped_lock lock{mutex};
        Snapshot result{{}, dropped};
        for (const auto& entry : entries)
            if (entry.token)
                result.entries.push_back(entry);
        return result;
    }

private:
    std::mutex mutex;
    std::array<Entry, 256> entries{};
    std::uint64_t sequence{}, dropped{};
};
inline Recorder recorder;
inline std::uint64_t Begin(Kind kind, std::uint64_t key, std::string_view label,
                           std::span<const std::uint64_t> args = {}, std::uint64_t pc = 0,
                           std::uint64_t lr = 0) {
    return Enabled() ? recorder.Begin(kind, key, label, args, pc, lr) : 0;
}
inline void End(std::uint64_t token, std::uint64_t result = 0) {
    if (token)
        recorder.End(token, result);
}
class Scope {
public:
    Scope(Kind kind, std::uint64_t key, std::string_view label,
          std::span<const std::uint64_t> args = {})
        : token{Begin(kind, key, label, args)} {}
    ~Scope() {
        End(token);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::uint64_t token;
};
} // namespace NxboxWait
