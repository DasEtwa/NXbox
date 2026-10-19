// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstring>
#include <iostream>

#include "video_core/renderer_d3d12/d3d12_state_cache.h"

int main() {
  using namespace D3D12;
  StateInputCache<GraphicsStateInputs> cache;
  GraphicsStateInputs inputs{};
  assert(!cache.Matches(inputs));
  cache.Store(inputs);
  assert(cache.Matches(inputs));
  const auto check = [&](auto mutate) {
    auto next = inputs;
    mutate(next);
    assert(!cache.Matches(next));
    assert(cache.Matches(inputs));
    cache.Store(next);
    assert(cache.Matches(next));
    cache.Store(inputs);
  };
  check([](auto &in) { ++in.generation; });
  for (size_t i = 0; i < inputs.hashes.size(); ++i)
    check([i](auto &in) { ++in.hashes[i]; });
  for (size_t i = 0; i < inputs.formats.colors.size(); ++i)
    check([i](auto &in) { ++in.formats.colors[i]; });
  check([](auto &in) { ++in.formats.depth; });
  check([](auto &in) { ++in.formats.sample_count; });
  check([](auto &in) { ++in.topology; });
  check([](auto &in) { ++in.zeta_enable; });
  check([](auto &in) { in.indexed = !in.indexed; });
  for (size_t i = 0; i < inputs.tessellation_enables.size(); ++i)
    check([i](auto &in) { ++in.tessellation_enables[i]; });
  cache.Invalidate();
  assert(!cache.Matches(inputs));

  // Padding in FramebufferFormats must never cause a false miss or an undefined
  // read.
  GraphicsStateInputs a, b;
  std::memset(&a, 0x55, sizeof(a));
  std::memset(&b, 0xAA, sizeof(b));
  const auto initialize = [](auto &in) {
    in.generation = 1;
    in.hashes.fill(2);
    in.formats.colors.fill(3);
    in.formats.depth = 4;
    in.formats.sample_count = 1;
    in.topology = 5;
    in.zeta_enable = 1;
    in.indexed = false;
    in.tessellation_enables.fill(0);
  };
  initialize(a);
  initialize(b);
  assert(a == b);

  StateInputCache<DynamicStateInputs> dynamic;
  DynamicStateInputs before{inputs, 1280, 720, 4, &cache};
  dynamic.Store(before);
  assert(dynamic.Matches(before));
  auto after = before;
  after.width = 1920;
  assert(!dynamic.Matches(after));
  after = before;
  after.height = 1080;
  assert(!dynamic.Matches(after));
  after = before;
  ++after.converted_topology;
  assert(!dynamic.Matches(after));
  after = before;
  after.pipeline = &dynamic;
  assert(!dynamic.Matches(after));
  after = before;
  ++after.graphics.generation;
  assert(!dynamic.Matches(after));
  // A command/channel/rescale invalidation remains visible independently to all
  // consumers.
  cache.Store(before.graphics);
  assert(!cache.Matches(after.graphics));
  dynamic.Store(after);
  assert(!cache.Matches(after.graphics));
  // Pending pipelines do not become pointers cached by this input cache; the
  // caller still goes through guest_graphics / async Lookup when
  // last_graphics_pipeline is null.
  std::cout << "state cache inputs: all fields, padding, invalidation and "
               "independent consumers OK\n";
}
