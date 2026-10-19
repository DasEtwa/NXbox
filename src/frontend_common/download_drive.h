// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace DownloadDrive {

// Shader caches, saves, logs and .nsz conversions write to the internal drive, so a download
// never takes its last GiB: a Shop download had filled it to a few MiB on the console.
inline constexpr std::uint64_t InternalReserve = 1ull << 30;

struct Candidate {
    std::uint64_t have = 0;            // bytes of this download already on the drive
    std::optional<std::uint64_t> free; // empty when the drive could not be queried
};

// Whether a drive with `free` bytes takes the `need` bytes still to download.
constexpr bool Fits(std::optional<std::uint64_t> free, std::uint64_t need, bool internal) {
    return free && *free >= need && *free - need >= (internal ? InternalReserve : 0);
}

// The removable drive a download of `total` bytes continues on: one that already holds part of
// it, otherwise the one with the most free space. Taking the first that fit put downloads on a
// nearly full stick ahead of an empty HD.
inline std::optional<std::size_t> Pick(const std::vector<Candidate>& drives, std::uint64_t total) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < drives.size(); ++i) {
        const auto& drive = drives[i];
        if (drive.have > total || !Fits(drive.free, total - drive.have, false))
            continue;
        if (!best) {
            best = i;
            continue;
        }
        const auto& current = drives[*best];
        const bool resumes = drive.have > 0;
        if (resumes != (current.have > 0) ? resumes : *drive.free > *current.free)
            best = i;
    }
    return best;
}

} // namespace DownloadDrive
