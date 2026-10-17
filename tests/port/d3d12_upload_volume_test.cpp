// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_index_converter.h"
#include "video_core/renderer_d3d12/d3d12_upload_cache.h"
#include <array>
#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
  using namespace D3D12;
  assert(UniformViewSize(0) == 0);
  assert(UniformViewSize(1) == 256);
  assert(UniformViewSize(256) == 256);
  assert(UniformViewSize(257) == 512);
  assert(UniformViewSize(65535) == 65536);
  assert(UniformViewSize(65536) == 65536);
  assert(UniformViewSize(65537) == 0);
  assert(UniformViewSize(UINT32_MAX) == 0);

  using Bytes = std::vector<unsigned char>;
  UniformCopyCache<Bytes, 2> cache;
  auto source = std::make_shared<Bytes>(1024, 7);
  auto copy = std::make_shared<Bytes>(256, 0);
  std::copy_n(source->begin() + 3, 17, copy->begin());
  cache.Insert(source, 1, 3, 17, copy);
  // A thousand unchanged draws reuse one immutable snapshot, including its zero
  // tail.
  for (unsigned i = 0; i < 1000; ++i) {
    auto found = cache.Find(source, 1, 3, 17);
    assert(found == copy && (*found)[16] == 7 && (*found)[17] == 0);
  }
  assert(!cache.Find(source, 2, 3,
                     17)); // CPU upload / same-state GPU write invalidates.
  assert(!cache.Find(source, 1, 4, 17));
  assert(!cache.Find(source, 1, 3, 18));
  auto other = std::make_shared<Bytes>(1024, 8);
  assert(!cache.Find(other, 1, 3, 17));
  auto changed = std::make_shared<Bytes>(256, 9);
  cache.Insert(source, 2, 3, 17, changed);
  assert(!cache.Find(source, 1, 3, 17));
  assert(cache.Find(source, 2, 3, 17) == changed);
  // Bound the cache, while a command-list lease preserves evicted data.
  cache.Insert(source, 2, 4, 17, copy);
  auto gpu_lease = changed;
  cache.Insert(other, 1, 0, 17, copy);
  assert(!cache.Find(source, 2, 3, 17));
  assert((*gpu_lease)[0] == 9);
  std::weak_ptr<Bytes> weak_source = source;
  source.reset();
  assert(weak_source.expired());
  assert(cache.Find(other, 1, 0, 17) == copy);

  // Snapshot hits refresh LRU order; replacing a version does not evict another
  // range.
  UniformCopyCache<Bytes, 2> lru;
  lru.Insert(other, 1, 0, 16, copy);
  lru.Insert(other, 1, 16, 16, copy);
  assert(lru.Find(other, 1, 0, 16));
  lru.Insert(other, 1, 32, 16, changed);
  assert(!lru.Find(other, 1, 16, 16));
  assert(lru.Find(other, 1, 0, 16));
  lru.Insert(other, 2, 0, 16, changed);
  assert(!lru.Find(other, 1, 0, 16));
  assert(lru.Find(other, 2, 0, 16) == changed);
  assert(lru.Find(other, 1, 32, 16) == changed);
  assert(!lru.Find({}, 1, 0, 16));

  // Reusing the exact source address must not revive an expired weak identity.
  alignas(unsigned) std::byte storage[sizeof(unsigned)];
  const auto make_source = [&] {
    auto pointer = std::construct_at(reinterpret_cast<unsigned *>(storage), 7u);
    return std::shared_ptr<unsigned>(
        pointer, [](unsigned *value) { std::destroy_at(value); });
  };
  UniformCopyCache<unsigned, 2> identities;
  auto identity = make_source();
  auto snapshot = std::make_shared<unsigned>(42);
  identities.Insert(identity, 1, 0, 4, snapshot);
  identity.reset();
  identity = make_source();
  assert(!identities.Find(identity, 1, 0, 4));
  identities.Insert(identity, 1, 0, 4, snapshot);
  assert(identities.Find(identity, 1, 0, 4) == snapshot);

  // Multiple immutable snapshots can share a resource without losing their offsets.
  UniformCopyCache<Bytes, 2> suballocated;
  suballocated.Insert(other, 1, 0, 16, copy, 512);
  suballocated.Insert(other, 1, 16, 16, copy, 1024);
  std::uint32_t copy_offset = 0;
  assert(suballocated.Find(other, 1, 0, 16, &copy_offset) == copy);
  assert(copy_offset == 512);
  assert(suballocated.Find(other, 1, 16, 16, &copy_offset) == copy);
  assert(copy_offset == 1024);

  suballocated.Clear();
  assert(!suballocated.Find(other, 1, 0, 16));
  assert(!suballocated.Find(other, 1, 16, 16));

  using namespace IndexConversion;
  assert(!RestartCanMatch(Format::Uint8, 256));
  assert(!RestartCanMatch(Format::Uint16, 0xffffffff));
  assert(RestartCanMatch(Format::Uint16, 65535));
  assert(RestartCanMatch(Format::Uint32, 0xffffffff));
  // Native identity fast path agrees with the real converter, including a
  // restart register which cannot match the narrow guest indices.
  const std::array<std::uint16_t, 6> indices{0, 1, 2, 65535, 4, 5};
  const auto input = std::as_bytes(std::span{indices});
  for (auto topology :
       {Topology::Points, Topology::Lines, Topology::Triangles}) {
    for (auto restart : {Restart{}, Restart{true, 0xffffffff}}) {
      assert(!NeedsConversion(topology, Format::Uint16, restart,
                              Provoking::Preserve));
      const auto layout = Analyze(topology, Format::Uint16, input, restart);
      std::vector<std::byte> output(layout.bytes);
      Convert(topology, Format::Uint16, input, output, restart,
              Provoking::Preserve);
      assert(
          std::equal(input.begin(), input.end(), output.begin(), output.end()));
    }
  }
  assert(NeedsConversion(Topology::Triangles, Format::Uint16, {true, 65535},
                         Provoking::Preserve));
  assert(NeedsConversion(Topology::Triangles, Format::Uint16, {},
                         Provoking::Last));
  assert(NeedsConversion(Topology::Triangles, Format::Uint8, {},
                         Provoking::Preserve));
  assert(NeedsConversion(Topology::TriangleStrip, Format::Uint16, {},
                         Provoking::Preserve));
  // Scan packed, potentially unaligned little-endian data. Compare the full
  // register.
  const std::array<std::byte, 7> packed{
      std::byte{0}, std::byte{0xff}, std::byte{0xff}, std::byte{0},
      std::byte{0}, std::byte{1},    std::byte{0}};
  const auto unaligned = std::span{packed}.subspan(1);
  assert(ContainsRestart(Format::Uint16, unaligned, {true, 65535}));
  assert(!ContainsRestart(Format::Uint16, unaligned, {true, 0xffffffff}));
  assert(!ContainsRestart(Format::Uint16, unaligned, {}));
  assert(!ContainsRestart(Format::Uint16, input, {true, 42}));
  assert(ContainsRestart(Format::Uint8, unaligned, {true, 255}));
  assert(!ContainsRestart(Format::Uint8, unaligned, {true, 256}));
  assert(!ContainsRestart(Format::Uint32, {}, {true, 0}));
  const std::array<std::uint32_t, 3> wide{0, 0xffffffff, 1};
  assert(ContainsRestart(Format::Uint32, std::as_bytes(std::span{wide}),
                         {true, 0xffffffff}));
  bool truncated = false;
  try {
    ContainsRestart(Format::Uint32, unaligned, {});
  } catch (const std::invalid_argument &) {
    truncated = true;
  }
  assert(truncated);
  bool invalid_format = false;
  try {
    ContainsRestart(static_cast<Format>(99), {}, {});
  } catch (const std::invalid_argument &) {
    invalid_format = true;
  }
  assert(invalid_format);
}
