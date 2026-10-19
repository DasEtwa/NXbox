// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "frontend_common/download_drive.h"

using DownloadDrive::Candidate;
using DownloadDrive::Fits;
using DownloadDrive::InternalReserve;
using DownloadDrive::Pick;

int main() {
    constexpr std::uint64_t GiB = 1ull << 30;
    constexpr std::uint64_t MiB = 1ull << 20;
    // The internal drive keeps a GiB free; removable drives may fill up completely.
    static_assert(Fits(2 * GiB, GiB, true));
    static_assert(!Fits(765 * MiB, 500 * MiB, true));
    static_assert(Fits(765 * MiB, 765 * MiB, false));
    static_assert(!Fits(std::nullopt, 0, false));
    static_assert(!Fits(GiB, GiB + 1, false));
    static_assert(InternalReserve == GiB);

    // The console's layout: a nearly full stick enumerated before an empty HD.
    const std::vector<Candidate> stick_then_hd{{0, 1300 * MiB}, {0, 1700 * GiB}};
    assert(Pick(stick_then_hd, 500 * MiB) == 1u);
    // Only the stick has room for nothing; the HD takes what the stick cannot.
    assert(Pick(stick_then_hd, 2 * GiB) == 1u);
    // A partial download is resumed where it is, even on the smaller drive.
    const std::vector<Candidate> partial_on_stick{{200 * MiB, 1300 * MiB}, {0, 1700 * GiB}};
    assert(Pick(partial_on_stick, 1 * GiB) == 0u);
    // ...unless that drive can no longer hold the rest.
    assert(Pick(partial_on_stick, 2 * GiB) == 1u);
    // A file bigger than the download is not a partial of it.
    const std::vector<Candidate> oversized{{3 * GiB, 10 * GiB}};
    assert(Pick(oversized, 2 * GiB) == std::nullopt);
    // Unknown free space never wins, and no room anywhere means no pick.
    const std::vector<Candidate> unknown{{0, std::nullopt}, {0, 100 * MiB}};
    assert(Pick(unknown, 50 * MiB) == 1u);
    assert(Pick(unknown, 200 * MiB) == std::nullopt);
    assert(Pick({}, 1) == std::nullopt);
}
