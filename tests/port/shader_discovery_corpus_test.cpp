// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Input is the reviewer's extract.py envs.bin; no guest shader bytes are
// checked in.
#include "video_core/shader_discovery_scanner.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <vector>
using namespace VideoCommon::ShaderDiscovery;
int main(int argc, char **argv) {
  assert(argc == 2);
  std::ifstream file(argv[1], std::ios::binary);
  assert(file);
  std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(file)), {});
  std::vector<std::uint8_t> image;
  std::set<std::uint64_t> wanted, found;
  size_t graphics{}, compute{}, matched{}, accepted_headers{};
  for (size_t pos = 0; pos < blob.size();) {
    assert(blob.size() - pos >= 20);
    std::uint32_t stage, bound, size;
    std::uint64_t hash;
    std::memcpy(&stage, blob.data() + pos, 4);
    std::memcpy(&bound, blob.data() + pos + 4, 4);
    std::memcpy(&hash, blob.data() + pos + 8, 8);
    std::memcpy(&size, blob.data() + pos + 16, 4);
    pos += 20;
    assert(size <= blob.size() - pos);
    const auto code = std::span{blob}.subspan(pos, size);
    const auto result = Recompute(code, bound == 2);
    assert(result.status == Status::Complete && result.hash == hash);
    ++matched;
    if (stage == 5) {
      ++compute;
    } else {
      ++graphics;
      assert(HeaderCandidate(code));
      ++accepted_headers;
      while (image.size() % 256)
        image.push_back(0);
      image.insert(image.end(), code.begin(), code.end());
      wanted.insert(hash);
    }
    pos += size;
  }
  assert(graphics && compute);
  Scanner scanner;
  while (!scanner.done)
    scanner.Step(image, 0x10000000, 2 * MaxCode,
                 [&](size_t, size_t, auto hash, bool) { found.insert(hash); });
  for (auto hash : wanted)
    assert(found.contains(hash));
  std::cout << "corpus: graphics=" << graphics << " compute=" << compute
            << " recompute_matches=" << matched
            << " accepted_graphics_headers=" << accepted_headers
            << " unique_hash_matches=" << wanted.size() << "/" << wanted.size()
            << " packed_headers=" << scanner.headers
            << " header_false_positives=" << scanner.headers - graphics
            << " extra_distinct_hashes=" << found.size() - wanted.size()
            << '\n';
}
