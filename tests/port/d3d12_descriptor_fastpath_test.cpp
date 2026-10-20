// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <vector>

// CPU descriptors are deliberately recycled at tiny capacities. Compare every
// resulting GPU table against the old full-key plan, including fallback writes.
int main() {
  std::mt19937 random{0x1234};
  std::size_t reuses{}, calls{}, fallbacks{}, resolutions{}, wraps{},
      submissions{};
  for (const unsigned capacity : {1, 3, 8, 32}) {
    D3D12::DescriptorSources<std::uintptr_t, 16> previous;
    std::vector<unsigned> previous_keys, previous_gpu;
    std::map<unsigned, unsigned> cache;
    std::vector<unsigned> cpu(capacity + 1);
    std::uint64_t generation{1}, tick{1}, previous_tick{};
    unsigned cursor{}, last_offset{};
    D3D12::RingAllocator ring{32};
    for (unsigned draw = 0; draw < 20000; ++draw) {
      if (random() % 11 == 0) {
        ++tick; // Submission invalidates GPU table reuse; keep newer ticks in
                // flight.
        ++submissions;
        if (tick > 2)
          ring.Retire(tick - 2);
        if (random() % 2 == 0) {
          cache.clear();
          cursor = 0;
          ++generation; // Frame-boundary CPU descriptor reset.
        }
      }
      auto keys = previous_keys;
      if (keys.empty() || random() % 7 == 0)
        keys.resize(1 + random() % 20); // Also exercise fixed-storage overflow.
      if (random() % 3 == 0)
        keys[random() % keys.size()] = random() % 31;
      D3D12::DescriptorSources<std::uintptr_t, 16> plan;
      plan.Reset(keys.size());
      for (unsigned slot = 0; slot < keys.size(); ++slot) {
        plan.Set(
            slot, previous,
            slot < previous_keys.size() && keys[slot] == previous_keys[slot],
            [&] { return generation; },
            [&]() -> std::uintptr_t {
              ++resolutions;
              auto found = cache.find(keys[slot]);
              if (found == cache.end()) {
                if (cursor == capacity) {
                  cache.clear();
                  cursor = 0;
                  ++generation;
                }
                cpu[++cursor] = keys[slot];
                found = cache.emplace(keys[slot], cursor).first;
              }
              return found->second;
            });
      }
      auto can_reuse = [&] {
        const bool same = plan.Current(generation)
                              ? previous.Matches(plan, generation)
                              : previous_keys == keys;
        return previous_tick == tick && same;
      };
      // Reserve can submit after the initial reuse check.
      while (!can_reuse() && !ring.CanAllocate(keys.size())) {
        const auto oldest = *ring.OldestTick();
        if (oldest == tick) {
          ++tick;
          ++submissions;
        }
        ring.Retire(oldest);
      }
      if (can_reuse()) {
        ++reuses;
        assert(previous_gpu == keys);
        continue;
      }
      const bool live = ring.Used() != 0;
      const auto offset = ring.Allocate(keys.size(), tick);
      assert(offset);
      if (*offset < last_offset && live)
        ++wraps; // Nonzero backward movement proves real wrap, not an
                 // empty-ring reset.
      last_offset = *offset;
      std::vector<unsigned> gpu;
      if (plan.Current(generation)) {
        ++calls;
        for (unsigned slot = 0; slot < keys.size(); ++slot)
          gpu.push_back(cpu[plan.handles[slot]]);
      } else {
        ++fallbacks;
        gpu = keys; // Legacy immediate writer consumes each source before
                    // recycling.
      }
      assert(gpu == keys);
      previous = plan;
      previous_keys = keys;
      previous_gpu = gpu;
      previous_tick = tick;
    }
  }
  assert(reuses && calls && fallbacks && wraps);
  std::cout << "80000 draws: reuses=" << reuses << " table_calls=" << calls
            << " fallbacks=" << fallbacks << " resolutions=" << resolutions
            << " ring_wraps=" << wraps << " submissions=" << submissions
            << '\n';
}
