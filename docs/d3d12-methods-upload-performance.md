# Uniform snapshot allocation and Maxwell method costs

## Changes

`CopyUniform` previously called `Context::CreateBuffer` on every immutable-snapshot
cache miss. Even a 256-byte CBV needed a native resource, pool bookkeeping,
completion collection, and potentially an aliasing barrier. The CPU upload path
already used persistent mapped staging pages; mapping per upload was not the
problem found in this path.

Uniform snapshots now share DEFAULT pages through `StagingRing`. These copies may
originate from GPU-written guest buffers, so substituting an UPLOAD resource would
require a readback. Pages start at 4 MiB, with 512-byte aligned suballocations and
a 64 MiB permanent capacity per ring. Snapshot cache entries retain both the page
and its offset. A page resets only after its fence has retired and cache, binding,
CPU, and command-list leases have all gone away. At capacity, snapshot cache
entries are evicted before waiting, so hot entries cannot pin every page forever.
The existing bounded-pressure
submit/wait and temporary allocation fallback remain in force. CPU upload pages
remain persistently mapped and use the same fence retirement rules.

Expected saving: one native resource per **page** instead of per snapshot miss;
up to 8192 small snapshots or 64 full 64 KiB snapshots fit in one 4 MiB page.
Retired, unleased pages need no new resource. This removes a concrete fixed cost
inside the measured 16.1 ms allocation phase; the millisecond saving needs an Xbox
capture because that phase also includes unchanged index-conversion allocations.
Shared DEFAULT pages still transition as whole buffers, so compare barrier costs
as well as allocation costs.

Incrementing Maxwell packets covering the sixteen CB_DATA registers now call the
memory manager once per contiguous packet fragment, instead of once per word.
Every register, shadow value (including Replay), offset increment, and dirty
update is preserved. Batching stops at the CB_DATA boundary or available command
words; IncreaseOnce, other engines, and the existing non-incrementing path keep
their dispatch behavior. Guest cached-write invalidation is still accumulated by
`WriteBlockCached` and published at the existing flush sites.

Expected saving: a full sixteen-word fragment eliminates fifteen memory-manager
write/translation/accumulator calls (93.75% fewer calls for that fragment).
Singletons and already-batched non-incrementing packets do not gain this saving.

Unchanged register values now skip the two dirty-table lookups and dirty-bit
updates. Method side effects still execute, including repeated draw/query triggers.
Changed registers and shadow writes retain their existing behavior. Expected
saving scales with the measured unchanged-register count; no Xbox milliseconds
are claimed before measuring it.

## Next capture

Enable `NXBOX_D3D12_FRAME_PROFILE`. Existing `D3D12_FRAME_PROFILE` output remains
aggregated over five seconds, with counts and `_per_frame` values. Read:

- `uniform_snapshot_requests` against `uniform_snapshot_new_resources`: snapshot
  misses versus successful native page/temporary resource creations.
- `staging_requests`, `staging_fast_requests`, `staging_new_resources`, and
  `staging_collections`: mapped upload/readback suballocation and retirement work.
- `uniform_copy_cache_hits` / `uniform_copy_cache_misses`,
  `forced_submits_staging_pressure`, `upload_allocation_cpu_ms`,
  `staging_collect_cpu_ms`, `barriers_cpu_ms`, and `copy_hazard_barriers`.

New `D3D12_MAXWELL_METHODS` lines use the same five-second reporting window:

- Classes: `dirty_flags`, `constant_buffer`, `draw_state` (plain register sink
  batches), `query_report`, `macro_upload`, and `draw_dispatch` (draw-manager calls).
- Each class reports `_count`, `_units`, `_skipped`, `_timed_calls`,
  `_estimated_ms`, and `_estimated_ms_per_frame`.
- Units count CB words or sink register writes; otherwise one unit is one call.
  `dirty_flags_skipped / dirty_flags_count` is the unchanged-register fraction.
  `constant_buffer_units / constant_buffer_count` is the mean batch length.
- Calls belong to their completion window, including calls that submit/report
  internally. Counts are exact. One call in 64 is timed; the estimated total extrapolates the
  sampled mean. Times are **inclusive wall time**, so query waits, draw-triggered
  rasterizer work, and dirty updates within sink batches can overlap other
  instrumentation. Do not sum these classes or treat them as exclusive frame CPU
  phases. Rare/heterogeneous methods need longer captures to stabilize estimates.
- Disabled profiling performs no clock reads, allocations, or locks in these
  method scopes. Enabled collection uses only recording-thread local counters.

The dominant class and its actual frame saving remain to be established on Xbox.
Compare the same P5R scene and MK8 race, including total GPU-thread CPU, GPU busy,
and guest fence latency, rather than inferring an FPS gain from call reduction.

## Validation

Port tests execute production allocator, snapshot, parser, shadow/dirty-register,
and CB-write logic with host stubs. They cover unmapped DEFAULT pages, thousands
of suballocations, live cache leases across retirement, pressure, cache offsets,
zero padding, immutable old copies, all shadow modes, and partial CB packets.
The host fixtures compile with warnings as errors and ASan/UBSan. Native MSVC/UWP
build and Xbox profiling require the Windows/Xbox environment.

The requested suite (`pytest tests/port -q --deselect
 tests/port/test_t13_compat_matrix.py`, using the supplied vpt executable) passed:
**503 passed, 57 skipped, 31 deselected, 1720 subtests passed**. One pre-existing
source assertion was updated to recognize the already-present
`NXBOX_D3D12_NO_INDEX_VALIDATED` gate; index production code was unchanged.
