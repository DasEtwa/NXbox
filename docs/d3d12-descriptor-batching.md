# Per-draw CPU audit and descriptor-copy experiment

This is a default-off experiment, not a demonstrated FPS fix. Set
`NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES=1` to group up to 32 CPU descriptor copies
into one `ID3D12Device::CopyDescriptors` call. The switch is read once with
`BisectEnabled`; restart the app after changing it. The startup diagnostic prints it.

## Evidence

Local logs under `~/.local/share/nxbox/artifacts/` were parsed as data. These are
existing runs on older builds, **not** measurements of this patch. Each row uses
one late interval with at least 60 frames (51 for BotW), not a whole-run average.
Times below are interval phase milliseconds multiplied by 1000 / interval draws,
i.e. microseconds per draw. Counters include compute/helper work in the interval;
these are normalization ratios, not isolated timings of a single graphics call.

| Log and line | Frames / draws | Descriptor copies/draw | Descriptor writes phase µs/draw | Recording µs/draw | Pipeline lookup µs/draw | Buffer cache bind µs/draw |
| --- | --- | --- | --- | --- | --- | --- |
| `botw-REUSE/diag.txt:1547` | 51 / 185745 | 0.908 | 0.993 | — | — | — |
| `compat-2026-10-18/tennis-711d-match/diag.txt:5465` | 119 / 116395 | 0.866 | 0.873 | 5.214 | 2.464 | 1.862 |
| `compat-2026-10-18/odyssey-708/diag.txt:859` | 168 / 87864 | 3.128 | 2.504 | 5.556 | 2.970 | 2.529 |
| `compat-2026-10-18/kirby-711c-profile/diag.txt:1423` | 72 / 81167 | 2.186 | 2.156 | 6.187 | 2.920 | 3.611 |

The last three rows respectively have 1.694/1.189/1.419 table reuses per draw and
5.395/4.465/7.053 root CBV binds per draw. `redundant_state_skips` in these older
captures is zero; do not treat their root binding counts as evidence that the
already-merged state suppression is ineffective. BotW has 3642 draws/frame but
only 3308 descriptor copies/frame in this interval. Descriptor copying alone
cannot plausibly explain its entire frame time. Its whole descriptor-writes phase
is 3.62 ms/frame, an upper bound on the savings from optimizing that phase in this
capture, before considering batching overhead. No matching sampler/PDB build pair
was established; no symbolized attribution is claimed.

## Full draw path audit

Line references are to the worktree after this patch; unchanged rasterizer lines
also apply to the baseline.

- `d3d12_rasterizer.cpp:2522`: `Draw` pauses conditional rendering, builds plain
  parameters, calls templated `PrepareDraw`, then ticks work. `DrawCommand` at
  line 148 invokes `DrawIndexedInstanced`/`DrawInstanced`; this is not a
  `std::function` allocation per draw.
- `d3d12_rasterizer.cpp:488`: flush guest caching, lock both buffer and texture
  caches, synchronize guest descriptors, update render targets, snapshot and
  validate attachments. `CurrentGraphicsPipeline` at line 603 performs pipeline
  lookup (`d3d12_pipeline_cache.cpp:1174`). Pipeline reconstruction/hashing remains
  measurable, but skipping it needs an independently proven invalidation contract.
- `d3d12_rasterizer.cpp:748`: visit active shader stages, build uniform masks,
  resolve guest texture/sampler handles, bind storage/texel ranges and image views.
  `GetSamplerId` at line 812 and descriptor-key hashing remain; the cache mutexes
  are held across setup, not acquired separately for every descriptor.
- `d3d12_rasterizer.cpp:896`: update graphics buffers, bind geometry and each
  stage, validate ranges. `buffer_cache/buffer_cache.h:409,1369` visits buffers and
  repeats on deletion; its nested `BufferCacheBind` phase absorbs most of the
  measured cost. The outer `graphics_buffers` residual is only ~0.06 µs/draw,
  **not** the total update cost. Skipping updates on unchanged bindings would miss
  guest writes, remapping and GPU-produced contents.
- `d3d12_rasterizer.cpp:945–1407`: resolve feedback hazards, geometry, vertex
  padding/repacking and resource state aliases. Odyssey's vertex-copy phase alone
  is 3.813 µs/draw; it needs a separate format/range-specific investigation.
- `d3d12_buffer_cache.cpp:449`: uniform snapshots use a content-version/range cache
  and pooled allocation, not unconditional map/unmap. Persistent mappings are made
  at resource creation (`d3d12_context.cpp:1455,1510`). Tennis/Odyssey/Kirby snapshot
  requests per draw are 0.003/0/0.548; staging requests are 0.138/0.298/0.954.
- `d3d12_rasterizer.cpp:1407–1756`: release the buffer lock, rebuild complete
  descriptor keys, reserve capacity (rechecking submit tick), and reuse identical
  whole tables. `d3d12_descriptor.cpp:278` handles table misses; `WriteCached`
  checks destination contents and hashes the CPU descriptor cache. Before this
  patch each changed slot invokes `CopyDescriptorsSimple(1, ...)`. Scratch vectors
  retain capacity; keys and `shared_ptr` resource references still churn per draw.
- `d3d12_rasterizer.cpp:1758`: release texture lock, bind heaps and transition
  collected resources. `Context::TransitionBatch` retains resources, skips unchanged
  states and batches real barriers; write-state content versions still advance.
  `Context::Retain` deduplicates leases per recording. Removing these calls would
  threaten resource lifetime and uniform snapshot coherence.
- `d3d12_rasterizer.cpp:1776–1918`: cached root signature/PSO/topology/tables/CBVs,
  targets, viewport/scissor, IA vertex/index views and root constants, then query
  and stream-output setup, actual draw, feedback copyback and cleanup. Existing
  state suppression remains untouched. No new root signature or barrier bypass.

## Change and safety boundaries

Only graphics draw-table misses opt into batching. Compute and other direct
`WriteCached` callers retain the immediate path. Full table hits still return
before any batching work. Sources remain CPU-only heap descriptors; destinations
remain the same newly reserved shader-visible slots. Null range-size arrays mean
one descriptor per source/destination range, so skipped destination slots and
noncontiguous sources are preserved. View and sampler batches are separate.

Flush on 32 entries, before CPU-cache rollover, at the end of a table, and on an
exception before unwinding the table build. The last condition is needed because
`slot_contents` can already describe queued writes. No submit occurs within cache
lookup/copy; reservation and any submit happen before batching. No source handle
survives heap recycling or a table boundary. The helper uses fixed stack storage;
it adds no heap allocation or `std::function`. Existing resource ownership remains.

`descriptor_copies` continues to count descriptors, preserving historical meaning.
New `descriptor_copy_calls` counts API calls from the cached descriptor path, both
immediate and batched. It is not a count of all descriptor APIs in the renderer.

## Local proof and limitations

`python3 -m unittest discover -s tests/port -p 'test_d3d12_descriptor_batch.py'`
compiles the actual portable helper with clang++ C++20, warnings as errors,
AddressSanitizer and UndefinedBehaviorSanitizer. The deterministic synthetic
counting microbenchmark includes empty flushes, sparse destinations, repeated
sources, partial/full batches and CPU cache rollover at capacities 1/3/32/64.
All 480 copied values agree with the immediate baseline; API calls change from
480 to 400/140/25/22 respectively. These are **not game captures or driver timings**.
Python contract checks cover default-off wiring, rollover/exception flushes, table
reuse, API range arguments and counters. Existing descriptor-ring, table-reuse,
redundant-state and robust-profile tests pass. Windows compilation, real descriptor
validation and rendered correctness remain for CI and the tester.

The prescribed `git clang-format origin/main -- ...` could not create its temporary
index under the main clone's `.git/worktrees/wt-cpu2` (sandbox denied it). Instead,
clang-format was applied only to changed line ranges, without touching Git metadata;
the new test was fully formatted, and the Python test used locally cached ruff.

## Console A/B recipe

The lead must build/package the uncommitted patch first; no CI/package run was
requested or generated here. In `LocalState/nxbox_env.txt`, preserve existing game
settings and set:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES=0
```

Fully restart. Use a fixed Odyssey scene first (highest copy ratio here), then the
same BotW scene/save, Tennis match and Kirby heavy area used in baseline testing.
Warm shaders for 30 seconds, hold camera/input constant and collect 60 seconds of
profile intervals. Do not take screenshots during the measurement. Repeat after
fully restarting with only `NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES=1`, then repeat 0
again to detect scene drift. Verify the startup bisect diagnostic for each run.
Compare interval sums divided by total frames and by draws, not individual spikes.

Required mechanism result: `descriptor_copy_calls / draws` drops in scenes with
multi-descriptor table misses; with 0, calls equal `descriptor_copies` for this
path. `descriptor_copies`, writes, reuses, draws, barriers and submits should remain
comparable. No root-bind or allocation reduction is claimed. Desired performance
result: lower `descriptor_writes_cpu_ms / draws`, lower total GPU-thread time per
frame and no FPS regression. More copies per API call does not guarantee a faster
driver path; leave OFF if the CPU timing does not improve.

Inspect matching-scene visuals outside the measured window, including textures,
HUD, shadows, transparency and scene transitions. Reject on skipped draws, device
loss, descriptor/debug errors or changed visuals. For a separate correctness run,
use `NXBOX_D3D12_DEBUG=1` where the debug layer is available; do not compare its
performance against a non-debug run. This experiment does not establish a solution
to the larger per-draw CPU bottleneck.
