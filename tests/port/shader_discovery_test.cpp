// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/shader_discovery_scanner.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static std::vector<std::string> logs;
template <typename... Args> void TestLog(const char *format, const Args &...) {
  logs.emplace_back(format);
}
// Compile the runtime as well, replacing only the logging dependency on the
// host.
#include "video_core/shader_discovery.cpp"
using namespace VideoCommon::ShaderDiscovery;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t BranchA = 0xE2400FFFFF87000FULL;
constexpr std::uint64_t BranchB = 0xE2400FFFFF07000FULL;
constexpr std::uint64_t Exit = 0xE30000000007000FULL;
void Put(Bytes &bytes, size_t offset, std::uint64_t word) {
  assert(offset + 8 <= bytes.size());
  std::memcpy(bytes.data() + offset, &word, 8);
}
void Header(Bytes &bytes, size_t offset = 0) { Put(bytes, offset, 0x02060461); }
struct Hit {
  size_t offset, length;
  std::uint64_t hash;
  bool branch;
};
std::vector<Hit> Scan(const Bytes &bytes, std::uint64_t address = 0,
                      size_t slice = 2 * MaxCode) {
  Scanner scanner;
  std::vector<Hit> hits;
  size_t loops{};
  while (!scanner.done) {
    scanner.Step(bytes, address, slice,
                 [&](size_t offset, size_t length, auto hash, bool branch) {
                   hits.push_back({offset, length, hash, branch});
                 });
    assert(++loops < 100000);
  }
  return hits;
}
int main(int argc, char **) {
  if (argc > 1) {
    // Read once: changing the environment after startup has no effect.
    unsetenv("NXBOX_SHADER_DISCOVERY_TRACE");
    assert(!Enabled());
    setenv("NXBOX_SHADER_DISCOVERY_TRACE", "1", 1);
    assert(!Enabled());
    Reset();
    EndFrame();
    Shutdown();
    assert(!state && logs.empty());
    std::cout << "discovery disabled: no state or logs; environment latched\n";
    return 0;
  }
  assert(Recompute({}, false).status == Status::Incomplete);
  // Published CityHash64 golden vectors, independent of the scanner.
  assert(Common::CityHash64("", 0) == 0x9ae16a3b2f90404fULL);
  assert(Common::CityHash64("a", 1) == 0xb3454265b6df75e3ULL);
  assert(Common::CityHash64("abc", 3) == 0x24a5b3a074e7f369ULL);
  for (auto terminator : {BranchA, BranchB, Exit}) {
    for (size_t offset :
         {size_t{0}, size_t{0xff8}, size_t{0x1000}, MaxCode - 8}) {
      Bytes bytes(MaxCode + 16, 0x55);
      Put(bytes, offset, terminator);
      const auto result = Recompute(bytes, false);
      const size_t expected_length = offset + (terminator == Exit ? 8 : 0);
      assert(result.status == Status::Complete &&
             result.size == expected_length);
      assert(result.hash ==
             Common::CityHash64(reinterpret_cast<const char *>(bytes.data()),
                                expected_length));
      Put(bytes, offset + 8, 123); // CachedSizeBytes' extra word is excluded.
      assert(Recompute(bytes, false).hash == result.hash);
      if (terminator == Exit)
        assert(Recompute(bytes, true).status == Status::Unsupported);
    }
  }
  // Real vertex and fragment SPH words from the reviewed cache corpus.
  for (auto word : {0x02060461U, 0x00065462U}) {
    Bytes header(HeaderSize);
    Put(header, 0, word);
    assert(HeaderCandidate(header));
  }
  for (auto word : {0x02060462U, 0x00065461U, 0x00065442U, 0x02260461U}) {
    Bytes header(HeaderSize);
    Put(header, 0, word);
    assert(!HeaderCandidate(header));
  }
  Bytes maximum(MaxCode + 8, 0);
  Put(maximum, MaxCode, BranchA); // Outside the cache scan's maximum.
  assert(Recompute(maximum, false).status == Status::Unsupported);
  Bytes bytes(0x180, 0);
  Header(bytes);
  Header(bytes, 8); // Overlapping SPH candidates.
  Put(bytes, 0x100, Exit);
  Put(bytes, 0x108, BranchB);
  auto hits = Scan(bytes, 0, 512);
  assert(hits.size() == 4);
  assert(hits[0].offset == 0 && hits[0].length == 0x108 && !hits[0].branch);
  assert(hits[1].offset == 0 && hits[1].length == 0x108 && hits[1].branch);
  assert(hits[2].offset == 8 && hits[3].offset == 8);
  for (const auto &hit : hits) {
    const auto result =
        Recompute(std::span{bytes}.subspan(hit.offset), hit.branch);
    assert(result.hash == hit.hash && result.size == hit.length);
  }
  assert(Scan(Bytes{}).empty());
  assert(Scan(Bytes(HeaderSize - 1)).empty());
  Bytes truncated(HeaderSize);
  Header(truncated);
  assert(Scan(truncated).empty());
  Bytes invalid(128);
  Put(invalid, 0, ~0ULL);
  Put(invalid, 120, BranchA);
  assert(Scan(invalid).empty());
  Bytes edge(HeaderSize + 8);
  Header(edge);
  Put(edge, HeaderSize, BranchA);
  assert(Scan(edge).size() == 1);
  Bytes unaligned(edge.size() + 1);
  std::copy(edge.begin(), edge.end(), unaligned.begin() + 1);
  assert(Scan(unaligned, 7).size() ==
         1); // Absolute alignment, not host pointer alignment.
  assert(Scan(unaligned, 0).empty());
  assert(Scan(edge, UINT64_MAX - 16).empty());
  // Crossing pages and small scan slices preserves the complete hash range.
  Bytes paged(0x2100);
  Header(paged, 0xff8);
  Put(paged, 0x2080, BranchA);
  auto large_hits = Scan(paged, 0, 8192);
  assert(large_hits.size() == 1 && large_hits[0].offset == 0xff8);
  assert(large_hits[0].length == 0x1088);
  Scanner generation;
  generation.Step(paged, 0, 4096, [](auto...) {});
  generation =
      {}; // New immutable upload generation: never resume an old candidate.
  assert(!generation.candidate && generation.cursor == 0);
  Scanner unsupported_scan;
  Bytes unterminated(MaxCode);
  Header(unterminated);
  while (!unsupported_scan.done)
    unsupported_scan.Step(unterminated, 0, 2 * MaxCode,
                          [](auto...) { assert(false); });
  assert(unsupported_scan.unsupported >= 1);
  auto records = std::make_unique<Records>();
  auto *first = records->Get(1, 10, false);
  first->seen = 1;
  first->used = 12;
  assert(records->Get(1, 10, false) == first);
  assert(records->Get(2, 10, false)->seen ==
         -1); // Reused address space cannot inherit history.
  assert(records->Get(1, 10, true)->seen == -1); // Compute is independent.
  for (size_t i = 0; i < RecordCapacity; ++i)
    records->Get(3, i, false);
  assert(!records->Get(4, 999999, false) && records->overflow > 0);
  assert(LeadBin(0) == 0 && LeadBin(0.1) == 1 && LeadBin(1) == 2);
  assert(LeadBin(10) == 3 && LeadBin(60) == 3 && LeadBin(60.01) == 4);
  auto population_records = std::make_unique<Records>();
  for (size_t i = 0; i < 5; ++i) {
    auto *sample = population_records->Get(0, i, false);
    sample->seen = 0;
    constexpr double leads[]{0.01, 0.1, 1, 10, 61};
    sample->used = leads[i];
  }
  population_records->Get(0, 10, false)->used = 1; // Missing.
  auto *late = population_records->Get(0, 11, false);
  late->used = 1;
  late->seen = 2;
  auto *equal = population_records->Get(0, 12, false);
  equal->seen = equal->used = 1; // Strictly earlier only.
  population_records->Get(0, 13, true)->used = 1;
  const auto population = Count(*population_records, false);
  assert(population.used == 8 && population.before == 5);
  for (size_t i = 0; i < 5; ++i)
    assert(population.bins[i] == 1);
  assert(population.bins[5] == 3);
  assert(Count(*population_records, true).used == 1);
  for (auto bin : Count(*population_records, true).bins)
    assert(bin == 0);
  setenv("NXBOX_SHADER_DISCOVERY_TRACE", "1", 1);
  Reset();
  assert(state && Enabled());
  Base(1, 0x10000, false);
  for (size_t i = 0; i < 17; ++i)
    Base(2, i, false);
  assert(state->region_overflow == 2);
  for (size_t i = 0; i < 5; ++i)
    OwnedUpload(1, 0x10000, edge);
  assert(state->upload_overflow == 1);
  const auto hash = Recompute(edge, false).hash;
  // Discovery uses upload capture time, including when scanning happens after
  // demand.
  Demand(1, 0x10000, hash, 0, false);
  const auto used = state->records.Get(1, hash, false)->used;
  Demand(1, 0x20000, hash, 0, false);
  assert(state->records.Get(1, hash, false)->used == used);
  state->last_scan -= std::chrono::seconds{1};
  state->last_summary -= std::chrono::seconds{61};
  EndFrame();
  auto *record = state->records.Get(1, hash, false);
  assert(record->seen < record->used && state->slices == 1);
  assert(state->accepted == 1 && state->frame == 1);
  assert(std::any_of(logs.begin(), logs.end(), [](const auto &line) {
    return line.find("summary kind=") != std::string::npos;
  }));
  const auto old_work = state->scanned;
  EndFrame(); // Rate limit, even with queued snapshots.
  assert(state->scanned == old_work);
  Demand(1, 0x10000, 0x12345, 0, true);
  assert(state->records.Get(1, 0x12345, true)->seen == -1);
  Shutdown();
  assert(!state);

  // Multiple candidates in one upload drain in the same 250 ms slice.
  Reset();
  Base(1, 0x10000, false);
  Bytes pool(300 * 128);
  for (size_t i = 0; i < 300; ++i) {
    Header(pool, i * 128);
    Put(pool, i * 128 + 80, i + 123);
    Put(pool, i * 128 + 88, BranchA);
  }
  OwnedUpload(1, 0x10000, pool, true);
  const auto captured = state->uploads[0].captured;
  const auto log_count = logs.size();
  Demand(1, 0x10000, Recompute(pool, true).hash, 0, false);
  assert(logs.size() == log_count); // Demand/base only buffer events.
  state->last_scan -= std::chrono::seconds{1};
  EndFrame();
  assert(!state->uploads[0].occupied && state->accepted == 300);
  assert(state->bulk_sph == 1 && state->bulk_uploads == 1);
  for (const auto &r : state->records.entries)
    if (r.occupied && r.seen >= 0)
      assert(r.seen == captured);
  UnsupportedHash(1, 999);
  Demand(1, 0x10000, 999, 0, false);
  assert(Count(state->records, false).unsupported == 1);
  assert(Count(state->records, false).bins[5] == 0);
  Shutdown();

  Reset();
  Base(1, 0x10000, false);
  Demand(1, 0x10000, hash, 0, false);
  OwnedUpload(1, 0x10000, edge);
  state->last_scan -= std::chrono::seconds{1};
  EndFrame();
  assert(state->records.Get(1, hash, false)->seen >=
         state->records.Get(1, hash, false)->used);
  assert(Count(state->records, false).bins[5] == 1);
  Shutdown();

  // No base at capture: report candidates rather than silently dropping them.
  Reset();
  OwnedUpload(1, 0x10000, edge);
  state->last_scan -= std::chrono::seconds{1};
  EndFrame();
  assert(state->prebase == 1 && state->scalar_sph == 1);
  Shutdown();

  // Incremental live read, overlapping window, zero-filled unmapped tail and
  // CPU-written code without an upload notification. No per-candidate reads.
  Reset();
  Base(1, 0x10000, false);
  Tegra::MemoryManager memory;
  memory.bytes.resize(MaxCode + edge.size());
  std::copy(edge.begin(), edge.end(), memory.bytes.begin() + MaxCode - 8);
  for (int i = 0; state->live_windows < 1 && i < 100; ++i) {
    state->last_scan -= std::chrono::seconds{1};
    const auto before = state->scanned;
    EndFrame(&memory);
    assert(state->scanned - before <= 2 * MaxCode);
  }
  assert(state->live_windows >= 1 && state->accepted == 1);
  assert(memory.reads >= 32 && memory.max_read <= 65536);
  assert(state->records.Get(1, hash, false)->seen >= 0);
  assert(state->records.Get(1, hash, false)->live_seen >= 0);
  assert(state->records.Get(1, hash, false)->upload_seen < 0);
  const auto reads = memory.reads;
  EndFrame(&memory);
  assert(memory.reads == reads); // Rate limited.
  assert(state->regions[0].cursor >= MaxCode);
  Shutdown();
  Reset();
  Base(1, 0x10000, false);
  Base(1, 0x20000, false);
  assert(!state->regions[0].active && state->regions[1].active);
  Base(1, 0x10000, false);
  assert(state->regions[0].active && !state->regions[1].active);
  Bytes oversized(2 * MaxCode);
  OwnedUpload(1, 0x10000 + 32 * MaxCode, oversized, true);
  assert(state->bulk_uploads == 1 && state->upload_overflow == 1);
  assert(state->regions[0].extent >= 34 * MaxCode);
  Shutdown();
  std::cout << "discovery scanner/runtime: hash rules, bounds, slices, "
               "overlap, overflow, lifecycle OK\n";
}
