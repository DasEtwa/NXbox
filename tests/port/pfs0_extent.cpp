// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/file_sys/pfs0_extent.h"

namespace {

using FileSys::MeasurePfs0;
using FileSys::Pfs0Span;

// An update NSP the way real dumps lay it out: NCAs first, the ticket and its
// certificate last. The sizes and offsets are those of a 527 MB update whose
// tail was lost by an interrupted copy.
constexpr std::uint64_t HeaderSize = 0x200;
const std::vector<Pfs0Span> UpdateNsp{
    {"program.nca", HeaderSize, 523858944},
    {"control.nca", HeaderSize + 523858944, 2925056},
    {"legal.nca", HeaderSize + 526784000, 175104},
    {"meta.cnmt.nca", HeaderSize + 526959104, 6656},
    {"0100f2c0115b68000000000000000010.tik", HeaderSize + 526965760, 704},
    {"0100f2c0115b68000000000000000010.cert", HeaderSize + 526966464, 1792},
};
constexpr std::uint64_t UpdateNspSize = HeaderSize + 526966464 + 1792;

} // namespace

int main() {
  int failures = 0;
  const auto check = [&](bool condition, const char *what) {
    if (!condition) {
      std::cerr << "FAILED: " << what << '\n';
      ++failures;
    }
  };

  // A complete file, and one with trailing padding, miss nothing.
  {
    const auto whole = MeasurePfs0(UpdateNsp, UpdateNspSize);
    check(!whole.Truncated(), "a complete NSP is not truncated");
    check(whole.required_size == UpdateNspSize,
          "required size is the end of the last entry");
    check(!MeasurePfs0(UpdateNsp, UpdateNspSize + 4096).Truncated(),
          "padding is harmless");
  }

  // Cut inside the meta NCA: it, the ticket and the certificate are gone; the
  // big NCAs are fine.
  {
    const auto cut = MeasurePfs0(UpdateNsp, HeaderSize + 526959104 + 100);
    check(cut.Truncated(), "a shortened NSP is truncated");
    check(cut.cut_off.size() == 3, "three entries are cut off");
    check(cut.cut_off.size() == 3 && cut.cut_off[0] == "meta.cnmt.nca",
          "the meta NCA is named first");
    check(cut.cut_off.size() == 3 &&
              cut.cut_off[1] == "0100f2c0115b68000000000000000010.tik",
          "the ticket is named");
    check(cut.required_size == UpdateNspSize,
          "required size does not depend on the file");
  }

  // Cut exactly between the meta NCA and the ticket: the ticket reads as 0
  // bytes.
  {
    const auto cut = MeasurePfs0(UpdateNsp, HeaderSize + 526965760);
    check(cut.cut_off.size() == 2 && cut.cut_off[0].ends_with(".tik") &&
              cut.cut_off[1].ends_with(".cert"),
          "only the ticket and its certificate are cut off");
  }

  // One byte short of the end cuts off the last entry, and only that one.
  {
    const auto cut = MeasurePfs0(UpdateNsp, UpdateNspSize - 1);
    check(cut.cut_off.size() == 1 && cut.cut_off[0].ends_with(".cert"),
          "one missing byte cuts off the last entry");
  }

  // Entries need not be sorted by offset, and an empty entry at the very end
  // fits.
  {
    const std::vector<Pfs0Span> shuffled{
        {"b", 100, 50}, {"a", 0, 100}, {"empty", 150, 0}};
    check(!MeasurePfs0(shuffled, 150).Truncated(), "unsorted entries that fit");
    check(MeasurePfs0(shuffled, 149).cut_off.size() == 2,
          "unsorted entries that do not fit");
  }

  // A corrupt header must not wrap around.
  {
    const std::vector<Pfs0Span> corrupt{{"x", UINT64_MAX - 1, 16}};
    check(MeasurePfs0(corrupt, 1u << 20).Truncated(),
          "an overflowing entry is cut off");
  }

  check(!MeasurePfs0({}, 0).Truncated(), "an empty container is complete");
  check(MeasurePfs0({}, 0).required_size == 0,
        "an empty container needs nothing");

  if (failures) {
    std::cerr << failures << " pfs0 extent checks failed\n";
  }
  return failures ? 1 : 0;
}
