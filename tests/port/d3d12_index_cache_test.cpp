// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_index_cache.h"
#include <cassert>
#include <memory>

using namespace D3D12::IndexConversion;
int main() {
  const CacheKey key{0x1000, 0x123456789abcdef0, 6, 2, 1, 5, 0xffff, 0, true};
  ConversionCache<int> cache{16};
  cache.Insert(key, 42, 8);
  const auto copy = key;
  assert(copy == key && CacheKeyHash{}(copy) == CacheKeyHash{}(key));
  assert(cache.Find(copy) && *cache.Find(copy) == 42);
  const auto miss = [&](CacheKey changed) {
    assert(!(changed == key));
    assert(!cache.Find(changed));
  };
  auto changed = key;
  ++changed.content_hash;
  miss(changed);
  changed = key;
  ++changed.address;
  miss(changed);
  changed = key;
  ++changed.count;
  miss(changed);
  changed = key;
  ++changed.first;
  miss(changed);
  changed = key;
  ++changed.format;
  miss(changed);
  changed = key;
  ++changed.topology;
  miss(changed);
  changed = key;
  ++changed.restart_index;
  miss(changed);
  changed = key;
  ++changed.provoking;
  miss(changed);
  changed = key;
  changed.restart_enabled = false;
  miss(changed);

  changed = key;
  ++changed.device_address;
  miss(changed); // Remapping within the same resource must revalidate.

  auto second = key;
  ++second.address;
  auto third = second;
  ++third.address;
  cache.Insert(second, 43, 8);
  assert(
      cache.Find(key)); // Touch the older entry: second must be evicted next.
  cache.Insert(third, 44, 8);
  assert(!cache.Find(second) && cache.Find(key) && cache.Find(third));
  assert(cache.Bytes() == 16 && cache.Size() == 2);
  cache.Insert(key, 45, 16); // Replacement and byte accounting.
  assert(cache.Bytes() == 16 && cache.Size() == 1 && *cache.Find(key) == 45);
  cache.Insert(second, 46, 17);
  cache.Insert(third, 47, 0);
  assert(cache.Bytes() == 16 && cache.Size() == 1);
  assert(!cache.Find(second) && !cache.Find(third));
  ConversionCache<int> empty{0};
  empty.Insert(key, 1, 1);
  assert(empty.Bytes() == 0 && empty.Size() == 0);
  ConversionCache<int> metadata_bound{10000};
  for (unsigned i = 0; i < 5000; ++i) {
    changed = key;
    changed.address = i;
    metadata_bound.Insert(changed, i, 1);
  }
  assert(metadata_bound.Size() == 4096 && metadata_bound.Bytes() == 4096);
  struct Source {
    std::uint64_t content_version{};
  };
  auto source = std::make_shared<Source>();
  ValidatedSnapshot<Source, int> snapshot{source, 0, 17};
  assert(snapshot.Matches(source) && snapshot.value == 17);
  ++source->content_version; // CPU upload or GPU write, including same-state
                             // writes.
  assert(!snapshot.Matches(source));
  snapshot.version = source->content_version;
  assert(snapshot.Matches(source));
  auto replacement = std::make_shared<Source>();
  replacement->content_version = snapshot.version;
  assert(!snapshot.Matches(replacement));
  std::weak_ptr<Source> weak_identity = source;
  source.reset();
  assert(weak_identity.expired());
  assert(!snapshot.Matches(replacement) && !snapshot.Matches({}));
  // An evicted allocation survives while a recorded draw still owns its lease.
  ConversionCache<std::shared_ptr<int>> leases{8};
  auto allocation = std::make_shared<int>(7);
  std::weak_ptr<int> weak = allocation;
  leases.Insert(key, allocation, 8);
  allocation.reset();
  auto draw_lease = *leases.Find(key);
  leases.Insert(second, std::make_shared<int>(8), 8);
  assert(!weak.expired() && *draw_lease == 7);
  draw_lease.reset();
  assert(weak.expired());
}
