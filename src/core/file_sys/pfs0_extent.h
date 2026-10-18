// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace FileSys {

// One file of a PFS0 container (an NSP): where its data starts in the container, and how long the
// container's header says it is.
struct Pfs0Span {
    std::string name;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// What a PFS0 container needs from the file that holds it.
struct Pfs0Extent {
    // Bytes from the start of the file to the end of the last entry.
    std::uint64_t required_size = 0;
    // The entries that end after the end of the file, in header order. Reading one of them returns
    // fewer bytes than the header promises, or none at all.
    std::vector<std::string> cut_off;

    [[nodiscard]] bool Truncated() const {
        return !cut_off.empty();
    }
};

// A PFS0 whose file is shorter than its header says (an interrupted copy or download) still parses:
// the entries before the cut read fine and the ones after it read as empty. That shows up far from
// the cause, as an empty ticket or an NCA with a bad header. This finds the cause.
[[nodiscard]] inline Pfs0Extent MeasurePfs0(const std::vector<Pfs0Span>& entries,
                                            std::uint64_t file_size) {
    Pfs0Extent extent;
    for (const Pfs0Span& entry : entries) {
        const bool overflows =
            entry.size > (std::numeric_limits<std::uint64_t>::max)() - entry.offset;
        const std::uint64_t end =
            overflows ? (std::numeric_limits<std::uint64_t>::max)() : entry.offset + entry.size;
        if (end > extent.required_size) {
            extent.required_size = end;
        }
        if (end > file_size) {
            extent.cut_off.push_back(entry.name);
        }
    }
    return extent;
}

} // namespace FileSys
