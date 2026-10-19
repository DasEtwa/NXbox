// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend_common/nsz_failure.h"
#include <cassert>

int main() {
  using namespace NszFailure;
  assert(Unchanged(1024, -100, 1024, -100));
  assert(!Unchanged(1024, -100, 2048, -100));
  assert(!Unchanged(1024, -100, 1024, -99));
  assert(!Unchanged(1024, -100, 2048, -99));
  assert(Unchanged(UINT64_MAX, INT64_MIN, UINT64_MAX, INT64_MIN));
  assert(Classify("game.ncz: zstd error: Data corruption detected at NCA "
                  "offset 0x4000") == Kind::Damaged);
  assert(Classify("the source may be corrupt") == Kind::Damaged);
  assert(Classify("HASH MISMATCH") == Kind::Damaged);
  assert(
      Classify(
          "the NCZBLOCK decompressed size does not match the section table") ==
      Kind::Damaged);
  assert(Classify(
             "the NCZBLOCK block count does not match the decompressed size") ==
         Kind::Damaged);
  assert(Classify("game.ncz: internal error, wrote 100 bytes of the NCA "
                  "instead of 200") == Kind::Damaged);
  // A zeroed header (unfinished download) is damage; a real container in another format is not.
  assert(Classify("corrupt header: not a PFS0 container") == Kind::Damaged);
  assert(Classify("not an NSZ package") == Kind::Other);
  assert(Classify("cannot open the file") == Kind::Other);
  assert(Classify("cannot create game.nsp.partial") == Kind::Other);
  assert(Classify("cannot rename the output") == Kind::Other);
  assert(ShouldRemember("Data corruption detected", false));
  assert(ShouldRemember("cannot open the file", false));
  assert(!ShouldRemember("cancelled", false));
  assert(!ShouldRemember("Data corruption detected", true));
  assert(!ShouldRemember("cannot open the file", true));
  assert(!ShouldRemember("disk full at offset 0x4000", false));
  assert(!ShouldRemember("", false));
}
