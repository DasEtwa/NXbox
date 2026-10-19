// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include "common/cityhash.h"
namespace VideoCommon::ShaderDiscovery {
constexpr size_t MaxCode = 0x100000;
constexpr size_t HeaderSize = 0x50;
enum class Status { Complete, Incomplete, Unsupported };
struct HashResult {
    Status status{Status::Incomplete};
    size_t size{};
    std::uint64_t hash{};
};
inline std::uint64_t Word(std::span<const std::uint8_t> bytes, size_t offset) {
    std::uint64_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
// Exact linear Analyze/TryFindSize semantics, including the header. No CFG fallback.
inline HashResult Recompute(std::span<const std::uint8_t> bytes, bool proprietary) {
    const size_t end = std::min(bytes.size(), MaxCode) & ~size_t{7};
    for (size_t i = 0; i < end; i += 8) {
        const auto word = Word(bytes, i);
        const bool branch = word == 0xE2400FFFFF87000FULL || word == 0xE2400FFFFF07000FULL;
        if (branch || (!proprietary && word == 0xE30000000007000FULL)) {
            const size_t size = i + (branch ? 0 : 8);
            return {Status::Complete, size,
                    Common::CityHash64(reinterpret_cast<const char*>(bytes.data()), size)};
        }
    }
    return {bytes.size() >= MaxCode ? Status::Unsupported : Status::Incomplete};
}
// Permissive heuristic: SPH has no magic. Validate candidates against demand hashes.
inline bool HeaderCandidate(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < HeaderSize)
        return false;
    std::uint32_t first;
    std::memcpy(&first, bytes.data(), sizeof(first));
    const auto type = (first >> 10) & 15;
    const auto sph_type = first & 31;
    const auto version = (first >> 5) & 31;
    return ((sph_type == 1 && type >= 1 && type <= 4) || (sph_type == 2 && type == 5)) &&
           version == 3 && (first & (3U << 21)) == 0;
}
// Resumable scan of one immutable upload or live window; never combines generations.
struct Scanner {
    size_t cursor{}, instruction{};
    bool candidate{}, done{}, exit_seen{};
    std::uint64_t incomplete{}, unsupported{}, headers{}, bytes_examined{};
    template <typename Found>
    void Step(std::span<const std::uint8_t> bytes, std::uint64_t address, size_t budget,
              Found&& found, size_t start_limit = std::numeric_limits<size_t>::max()) {
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - address) {
            done = true;
            ++unsupported;
            return;
        }
        const auto initial_work = bytes_examined;
        while (budget >= 8 && !done) {
            if (bytes_examined - initial_work >= 4096)
                return; // Clock check even in empty memory or a long candidate.
            if (!candidate) {
                if (cursor >= start_limit || cursor > bytes.size() ||
                    bytes.size() - cursor < HeaderSize) {
                    done = true;
                    break;
                }
                if ((address + cursor) % 8 != 0 || !HeaderCandidate(bytes.subspan(cursor))) {
                    ++cursor;
                    --budget;
                    ++bytes_examined;
                    continue;
                }
                candidate = true;
                instruction = 0;
                exit_seen = false;
                ++headers;
            }
            if (instruction >= MaxCode || bytes.size() - cursor < instruction + 8) {
                if (instruction >= MaxCode)
                    ++unsupported;
                else
                    ++incomplete;
                candidate = false;
                cursor += 8;
                continue;
            }
            const auto word = Word(bytes, cursor + instruction);
            const bool branch = word == 0xE2400FFFFF87000FULL || word == 0xE2400FFFFF07000FULL;
            const bool exit = !exit_seen && word == 0xE30000000007000FULL;
            const size_t length = instruction + (branch ? 0 : 8);
            if ((branch || exit) && budget < length + 8)
                break;
            instruction += 8;
            budget -= 8;
            bytes_examined += 8;
            if (branch || exit) {
                budget -= length;
                bytes_examined += length;
                found(cursor, length,
                      Common::CityHash64(reinterpret_cast<const char*>(bytes.data() + cursor),
                                         length),
                      branch);
                if (branch) {
                    candidate = false;
                    cursor += 8; // Preserve overlapping headers.
                } else {
                    exit_seen = true; // Continue for the proprietary-driver hash.
                }
                return; // Caller can check its clock between candidates.
            }
        }
    }
};
constexpr size_t RecordCapacity = 16384;
struct Record {
    std::uint64_t space{}, hash{};
    double seen{-1}, used{-1};
    bool compute{}, occupied{}, unsupported{};
    double upload_seen{-1}, live_seen{-1};
};
struct Records {
    std::array<Record, RecordCapacity> entries{};
    std::uint64_t overflow{};
    Record* Get(std::uint64_t space, std::uint64_t hash, bool compute) {
        size_t slot = (hash ^ (space * 0x9e3779b97f4a7c15ULL) ^ compute) % entries.size();
        for (size_t i = 0; i < entries.size(); ++i) {
            auto& entry = entries[(slot + i) % entries.size()];
            if (!entry.occupied) {
                entry = {space, hash, -1, -1, compute, true};
                return &entry;
            }
            if (entry.space == space && entry.hash == hash && entry.compute == compute)
                return &entry;
        }
        ++overflow;
        return nullptr;
    }
};
inline size_t LeadBin(double lead) {
    return lead < 0.1 ? 0 : lead < 1 ? 1 : lead < 10 ? 2 : lead <= 60 ? 3 : 4;
}
struct Population {
    std::uint64_t used{}, before{}, unsupported{};
    std::array<std::uint64_t, 6> bins{};
};
inline Population Count(const Records& records, bool compute, unsigned source = 0) {
    Population result;
    for (const auto& record : records.entries) {
        if (!record.occupied || record.compute != compute || record.used < 0)
            continue;
        ++result.used;
        if (compute || record.unsupported) {
            ++result.unsupported;
            continue;
        }
        const double seen = source == 1   ? record.upload_seen
                            : source == 2 ? record.live_seen
                                          : record.seen;
        if (seen >= 0 && seen < record.used) {
            ++result.before;
            ++result.bins[LeadBin(record.used - seen)];
        } else {
            ++result.bins[5];
        }
    }
    return result;
}
} // namespace VideoCommon::ShaderDiscovery
