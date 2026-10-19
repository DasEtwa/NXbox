// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/shader_discovery.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string_view>
#include "common/logging.h"
#include "video_core/memory_manager.h"
#include "video_core/shader_discovery_scanner.h"
#ifdef NXBOX_UWP
#include "eden_uwp/diagnostic.h"
#define DISCOVERY_LOG(...) EdenXbox::Diagnostic(fmt::format(__VA_ARGS__))
#else
#define DISCOVERY_LOG(...) LOG_INFO(Render, __VA_ARGS__)
#endif

namespace VideoCommon::ShaderDiscovery {
namespace {
using Clock = std::chrono::steady_clock;
struct Region {
    std::uint64_t space{}, address{};
    bool compute{}, occupied{};
    // Sweep the initial pool and extend it when uploads/demands expose higher offsets.
    std::uint64_t extent{16 * MaxCode}, cursor{};
    bool active{true};
    double last_sweep{};
};
struct Upload {
    std::array<std::uint8_t, MaxCode> bytes;
    std::uint64_t space{}, address{};
    size_t size{};
    double captured{};
    Scanner scanner{};
    bool occupied{}, bulk{}, sph{};
};
struct Event {
    enum Kind { BaseEvent, SeenEvent, UsedEvent } kind;
    std::uint64_t space{}, address{}, hash{}, frame{};
    double time{}, seen{};
    size_t length{};
    unsigned slot{};
    bool compute{}, live{}, bulk{}, branch{};
};
struct State {
    Clock::time_point start{Clock::now()}, last_scan{start}, last_summary{start};
    std::uint64_t frame{}, region_overflow{}, upload_overflow{}, scanned{}, slices{};
    std::uint64_t incomplete{}, unsupported{}, candidates{}, accepted{}, uploaded{};
    std::uint64_t prebase{}, scalar_uploads{}, bulk_uploads{}, scalar_sph{}, bulk_sph{};
    std::uint64_t live_bytes{}, live_windows{}, event_overflow{};
    double scan_ms{};
    std::array<Region, 16> regions{};
    std::array<Upload, 4> uploads{};
    Records records{};
    std::array<Event, 1024> events{};
    size_t event_count{}, next_region{};
    // One reusable window: candidate starts in the first MiB can read a full MiB.
    std::array<std::uint8_t, 2 * MaxCode> live{};
    size_t live_read{}, live_region{};
    Scanner live_scanner{};
    bool live_active{};
    double live_captured{};
    double Now() const {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
    void Queue(Event event) {
        if (event_count < events.size())
            events[event_count++] = event;
        else
            ++event_overflow;
    }
};
// All observations and ReadBlockUnsafe calls run on the GPU command thread.
std::unique_ptr<State> state;
static_assert(sizeof(State) < 8 * 1024 * 1024);
void FlushEvents() {
    for (size_t i = 0; i < state->event_count; ++i) {
        const auto& e = state->events[i];
        DISCOVERY_LOG("NXBOX_SHADER_DISCOVERY event={} kind={} space={} slot={} hash={:016x} "
                      "address={:016x} frame={} time={:.6f} seen={:.6f} bytes={} mode={} source={}",
                      e.kind == Event::BaseEvent   ? "base"
                      : e.kind == Event::SeenEvent ? "first_seen"
                                                   : "first_use",
                      e.compute ? "compute" : "graphics", e.space, e.slot, e.hash, e.address,
                      e.frame, e.time, e.seen, e.length, e.branch ? "self_branch" : "exit",
                      e.live   ? "live_region"
                      : e.bulk ? "bulk_upload"
                               : "scalar_upload");
    }
    state->event_count = 0;
}
void Summary(bool shutdown) {
    const auto& s = *state;
    size_t pending{}, active_regions{};
    double max_sweep_age{};
    std::uint64_t max_extent{};
    for (const auto& r : s.regions) {
        if (r.occupied && r.active && !r.compute) {
            ++active_regions;
            max_sweep_age = std::max(max_sweep_age, s.Now() - r.last_sweep);
            max_extent = std::max(max_extent, r.extent);
        }
    }
    for (const auto& u : s.uploads)
        pending += u.occupied;
    for (bool compute : {false, true}) {
        const auto p = Count(s.records, compute);
        const auto uploads = Count(s.records, compute, 1);
        const auto live = Count(s.records, compute, 2);
        DISCOVERY_LOG(
            "NXBOX_SHADER_DISCOVERY summary kind={} shutdown={} time={:.3f} first_used={} "
            "seen_before={} lead_lt_0_1={} lead_0_1_1={} lead_1_10={} lead_10_60={} "
            "lead_gt_60={} never_seen_before={} unsupported_used={} scanner_unsupported={} "
            "record_overflow={} region_overflow={} upload_overflow={} pending_uploads={} "
            "captured_upload_bytes={} scanned_work_bytes={} slices={} scan_ms={:.3f} "
            "candidates={} accepted={} incomplete={} candidates_before_base={} "
            "scalar_uploads={} bulk_uploads={} scalar_uploads_with_sph={} bulk_uploads_with_sph={} "
            "live_read_bytes={} live_windows={} event_overflow={} "
            "coverage=uploads_and_live_regions "
            "residency_coverage=sampled compute_scanner=unsupported upload_seen_before={} "
            "live_seen_before={} "
            "active_regions={} max_region_extent={} max_sweep_age_s={:.3f}",
            compute ? "compute" : "graphics", shutdown, s.Now(), p.used, p.before, p.bins[0],
            p.bins[1], p.bins[2], p.bins[3], p.bins[4], p.bins[5], p.unsupported, s.unsupported,
            s.records.overflow, s.region_overflow, s.upload_overflow, pending, s.uploaded,
            s.scanned, s.slices, s.scan_ms, s.candidates, s.accepted, s.incomplete, s.prebase,
            s.scalar_uploads, s.bulk_uploads, s.scalar_sph, s.bulk_sph, s.live_bytes,
            s.live_windows, s.event_overflow, uploads.before, live.before, active_regions,
            max_extent, max_sweep_age);
    }
}
void Found(std::uint64_t space, std::uint64_t address, double captured, bool live, bool bulk,
           size_t length, std::uint64_t hash, bool branch) {
    auto& s = *state;
    ++s.candidates;
    bool match{};
    for (const auto& r : s.regions)
        if (r.occupied && !r.compute && r.space == space && address >= r.address &&
            address - r.address <= UINT32_MAX)
            match = true;
    if (!match) {
        ++s.prebase;
        return;
    }
    auto* record = s.records.Get(space, hash, false);
    if (!record)
        return;
    auto& source_seen = live ? record->live_seen : record->upload_seen;
    if (source_seen < 0 || source_seen > captured)
        source_seen = captured;
    if (record->seen >= 0 && record->seen <= captured)
        return;
    record->seen = captured;
    ++s.accepted;
    s.Queue({Event::SeenEvent, space, address, hash, s.frame, captured, captured, length, 0, false,
             live, bulk, branch});
}
} // namespace
bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_SHADER_DISCOVERY_TRACE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}
void Reset() {
    if (!Enabled())
        return;
    state = std::make_unique<State>();
    DISCOVERY_LOG(
        "NXBOX_SHADER_DISCOVERY enabled scope=first_pipeline_demand "
        "scanner=uploads_and_live_regions "
        "regions=16 records=16384 uploads=4x1MiB live_window=2MiB live_stride=1MiB "
        "initial_region=16MiB scan_interval_ms=250 scan_work_limit=2MiB scan_soft_budget_ms=1");
}
void Base(std::uint64_t space, std::uint64_t address, bool compute) {
    if (!state)
        return;
    for (auto& r : state->regions)
        if (r.occupied && r.space == space && r.compute == compute)
            r.active = r.address == address;
    for (const auto& r : state->regions)
        if (r.occupied && r.space == space && r.address == address && r.compute == compute)
            return;
    for (auto& r : state->regions) {
        if (!r.occupied) {
            r = {space, address, compute, true};
            state->Queue({Event::BaseEvent, space, address, 0, state->frame, state->Now(), -1, 0, 0,
                          compute});
            return;
        }
    }
    ++state->region_overflow;
}
void OwnedUpload(std::uint64_t space, std::uint64_t address, std::span<const std::uint8_t> bytes,
                 bool bulk) {
    if (!state || bytes.empty())
        return;
    auto& s = *state;
    ++(bulk ? s.bulk_uploads : s.scalar_uploads);
    for (auto& r : s.regions) {
        if (r.occupied && r.space == space && address >= r.address &&
            address - r.address <= UINT32_MAX)
            r.extent =
                std::max(r.extent,
                         std::min(std::uint64_t{UINT32_MAX} + 1,
                                  address - r.address +
                                      std::min<std::uint64_t>(bytes.size(), UINT32_MAX) + MaxCode));
    }
    // Oversize uploads still trigger/extend live coverage; never copy unbounded input.
    if (bytes.size() > MaxCode || bytes.size() > UINT64_MAX - address) {
        ++s.upload_overflow;
        return;
    }
    for (auto& u : s.uploads) {
        if (!u.occupied) {
            u.space = space;
            u.address = address;
            u.size = bytes.size();
            u.captured = s.Now();
            u.scanner = {};
            u.occupied = true;
            u.bulk = bulk;
            u.sph = false;
            std::memcpy(u.bytes.data(), bytes.data(), bytes.size());
            s.uploaded += bytes.size();
            return;
        }
    }
    ++s.upload_overflow;
}
void UnsupportedHash(std::uint64_t space, std::uint64_t hash) {
    if (!state)
        return;
    for (bool compute : {false, true})
        if (auto* r = state->records.Get(space, hash, compute))
            r->unsupported = true;
}
void Demand(std::uint64_t space, std::uint64_t address, std::uint64_t hash, unsigned slot,
            bool compute) {
    if (!state)
        return;
    for (auto& r : state->regions) {
        if (r.occupied && r.space == space && r.compute == compute && address >= r.address &&
            address - r.address <= UINT32_MAX)
            r.extent = std::max(
                r.extent, std::min(std::uint64_t{UINT32_MAX} + 1, address - r.address + MaxCode));
    }
    if (auto* r = state->records.Get(space, hash, compute); r && r->used < 0) {
        r->used = state->Now();
        state->Queue({Event::UsedEvent, space, address, hash, state->frame, r->used, r->seen, 0,
                      slot, compute});
    }
}
void EndFrame(Tegra::MemoryManager* memory) {
    if (!state)
        return;
    auto& s = *state;
    ++s.frame;
    const auto now = Clock::now();
    if (now - s.last_scan >= std::chrono::milliseconds{250}) {
        s.last_scan = now;
        size_t work{};
        const auto available = [&] {
            return work < 2 * MaxCode && Clock::now() - now < std::chrono::milliseconds{1};
        };
        for (auto& u : s.uploads) {
            // Reserve roughly half the slice for live memory under upload pressure.
            while (u.occupied && available() && work < MaxCode &&
                   Clock::now() - now < std::chrono::microseconds{500}) {
                const auto before = u.scanner.bytes_examined;
                u.scanner.Step({u.bytes.data(), u.size}, u.address, 2 * MaxCode - work,
                               [&](size_t offset, size_t length, auto hash, bool branch) {
                                   Found(u.space, u.address + offset, u.captured, false, u.bulk,
                                         length, hash, branch);
                               });
                if (u.scanner.headers && !u.sph) {
                    u.sph = true;
                    ++(u.bulk ? s.bulk_sph : s.scalar_sph);
                }
                work += u.scanner.bytes_examined - before;
                if (u.scanner.done) {
                    s.incomplete += u.scanner.incomplete;
                    s.unsupported += u.scanner.unsupported;
                    u.occupied = false;
                }
                if (u.scanner.bytes_examined == before)
                    break; // Insufficient remaining hash budget: resume next slice.
            }
        }
        // Read small chunks, check time between reads/scan steps, preserve the immutable
        // window until its scan finishes. CPU writes can race this diagnostic snapshot;
        // only matching demand hashes count as evidence, never as a cache/prebuild input.
        while (memory && available()) {
            if (!s.live_active) {
                bool found{};
                for (size_t i = 0; i < s.regions.size(); ++i) {
                    const size_t index = (s.next_region + i) % s.regions.size();
                    const auto& r = s.regions[index];
                    if (r.occupied && r.active && !r.compute && r.space == memory->GetID() &&
                        r.address <= UINT64_MAX - r.cursor - s.live.size() &&
                        memory->IsWithinGPUAddressRange(r.address + r.cursor + s.live.size() - 1)) {
                        s.live_region = index;
                        s.next_region = (index + 1) % s.regions.size();
                        found = true;
                        break;
                    }
                }
                if (!found)
                    break;
                s.live_active = true;
                s.live_read = 0;
                s.live_scanner = {};
                s.live_captured = s.Now();
            }
            auto& r = s.regions[s.live_region];
            if (r.space != memory->GetID() || !r.active) {
                s.live_active = false;
                continue;
            }
            if (s.live_read < s.live.size()) {
                const size_t size =
                    std::min({size_t{65536}, s.live.size() - s.live_read, 2 * MaxCode - work});
                memory->ReadBlockUnsafe(r.address + r.cursor + s.live_read,
                                        s.live.data() + s.live_read, size);
                s.live_read += size;
                s.live_bytes += size;
                work += size;
                // Timestamp after the final read: never claim earlier residency.
                s.live_captured = s.Now();
                continue;
            }
            const auto before = s.live_scanner.bytes_examined;
            s.live_scanner.Step(
                s.live, r.address + r.cursor, 2 * MaxCode - work,
                [&](size_t offset, size_t length, auto hash, bool branch) {
                    Found(r.space, r.address + r.cursor + offset, s.live_captured, true, false,
                          length, hash, branch);
                },
                MaxCode);
            work += s.live_scanner.bytes_examined - before;
            if (s.live_scanner.done) {
                s.incomplete += s.live_scanner.incomplete;
                s.unsupported += s.live_scanner.unsupported;
                ++s.live_windows;
                r.cursor += MaxCode;
                if (r.cursor >= r.extent) {
                    r.last_sweep = s.Now();
                    r.cursor = 0;
                } // Periodic sweep also catches CPU writes without upload methods.
                s.live_active = false;
            }
            if (s.live_scanner.bytes_examined == before)
                break;
        }
        if (work) {
            ++s.slices;
            s.scanned += work;
            s.scan_ms += std::chrono::duration<double, std::milli>(Clock::now() - now).count();
        }
    }
    FlushEvents(); // No pipeline_mutex held here; never flush files on demand/base paths.
    if (now - s.last_summary >= std::chrono::seconds{60}) {
        Summary(false);
        s.last_summary = now;
    }
}
void Shutdown() {
    if (!Enabled() || !state)
        return;
    FlushEvents();
    Summary(true);
    state.reset();
}
} // namespace VideoCommon::ShaderDiscovery
#undef DISCOVERY_LOG
