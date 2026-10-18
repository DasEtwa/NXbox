// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <vector>

struct Result {
  std::vector<int> output;
  std::size_t calls{};
  std::size_t copies{};
};

// Deterministic recorded operations, not a capture from a game. Repeated
// sources, unchanged destination holes, >32 copies, table boundaries and CPU
// heap rollover.
struct Operation {
  std::size_t destination;
  std::size_t source;
};
Result Replay(bool batched, std::size_t heap_capacity) {
  Result result{std::vector<int>(600, -1)};
  std::array<int, 64> heap{};
  D3D12::DescriptorCopyBatch<std::size_t> batch;
  const auto copy = [&](std::size_t count, const auto *destinations,
                        const auto *sources) {
    ++result.calls;
    result.copies += count;
    for (std::size_t i = 0; i < count; ++i)
      result.output.at(destinations[i]) = heap.at(sources[i]);
  };
  std::size_t cursor{};
  for (std::size_t draw = 0; draw < 10; ++draw) {
    for (std::size_t slot = 0; slot < 47; ++slot) {
      if (slot % 7 == 0)
        continue;
      if (cursor == heap_capacity) {
        batch.Flush(
            copy); // Production flushes before clearing/recycling the cache.
        cursor = 0;
      }
      heap[cursor] = static_cast<int>(draw * 100 + slot);
      const Operation operation{draw * 60 + slot, cursor++};
      if (batched)
        batch.Add(operation.destination, operation.source, copy);
      else
        copy(1, &operation.destination, &operation.source);
      if (slot % 5 == 0) {
        // A cache hit can reuse the same CPU source more than once in a batch.
        const auto duplicate_destination = draw * 60 + 47 + slot / 5;
        if (batched)
          batch.Add(duplicate_destination, operation.source, copy);
        else
          copy(1, &duplicate_destination, &operation.source);
      }
    }
    batch.Flush(copy);
    const auto calls = result.calls;
    batch.Flush(copy);
    assert(calls == result.calls); // Empty tables do not issue an API call.
  }
  return result;
}
int main() {
  for (const auto capacity : {1u, 3u, 32u, 64u}) {
    const auto baseline = Replay(false, capacity);
    const auto batched = Replay(true, capacity);
    assert(baseline.output == batched.output);
    assert(baseline.copies == batched.copies);
    assert(batched.calls < baseline.calls);
    std::cout << "capacity=" << capacity << " copies=" << batched.copies
              << " calls=" << baseline.calls << "->" << batched.calls
              << " identical=true\n";
  }
}
