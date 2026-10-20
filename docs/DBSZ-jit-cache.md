# DBSZ JIT cache investigation

Evidence: read-only `compat-2026-10-18/{u-dbsz,u-mk8d,u-ody}/diag.txt` under
Miguel's NXbox artifacts directory. Window indices below start at zero and count
`GAME_STALL` records, including loading windows.

## What the capture actually proves

`NxboxStall::Take()` exchanges totals, maxima and calls with zero. The fields are
window deltas across the CPU cores, not cumulative counters. `jit` measures cache
miss processing; `jitemit` counts actual calls to the emitter. Concurrent emission
can cross a reporting boundary, so these counts can differ by a few calls.

**`jitflush=0/0/1` is one capacity evacuation.** The old implementation explicitly
recorded a duration of zero. Looking only at the first two fields hides it.
DBSZ has six such evacuations, discarding 1,519,715 resident block descriptors:

| Window | Capacity clears | Discarded blocks | JIT calls |
| --- | ---: | ---: | ---: |
| 4 | 1 | 258,360 | 139,938 |
| 13 | 1 | 250,017 | 78,574 |
| 19 | 1 | 253,243 | 41,482 |
| 31 | 1 | 255,251 | 81,971 |
| 36 | 2 | 502,844 | 140,569 |

There are 1,958,125 JIT calls across the 38 DBSZ windows. Core 0 reaches
132,684,538 / 134,217,728 bytes and core 1 reaches 130,496,240 / 134,217,728
in window 35. In the next window both reset to 26,189,682 and 45,387,128 bytes.
Later windows continue repopulating the evacuated caches even when no new clear
happens in that particular window.

MK8D and Odyssey have no capacity clears or range invalidations in their captures.
Their final core-0 occupancies are 91,509,538 and 63,452,948 bytes respectively.
Odyssey settles to almost no compilation; MK8D still discovers some code.

All captured DBSZ range requests, range-invalidated blocks, explicit clear
requests, instruction-cache operations and page-table invalidations are zero.
Consequently a write-triggered invalidation storm is not the explanation for this
capture. The old prefix histories track only 65,536 PCs/keys **per JIT**; most
later compilations are explicitly unknown, not proven repeats or new code.
It would be incorrect to label every one of the 1.96 million misses recompilation,
or claim the entire guest working set has been measured.

## Code paths

- `core/arm/dynarmic/nxbox_jit_policy.h` previously selected 128 MiB by default on
  UWP; 192 MiB required an environment opt-in. Desktop uses 512 MiB.
- In x64 A64 `Jit::Impl::GetBlock`, a miss with less than 1 MiB remaining sets
  `invalidate_entire_cache` and immediately applies it. `BlockOfCode::ClearCache`
  rewinds the cursor to the post-prelude start. `A64EmitX64::ClearCache` drops all
  descriptors, guest ranges, dispatch entries and fastmem patch records; RSB and
  restored disk records are also reset. There is no hot-block retention.
- `KProcess::InitializeInterfaces` owns one ARM interface per process/core.
  Switching guest threads on a core reuses that JIT. Migration can compile the
  same PC in another core's JIT, and FPCR/single-step variants have distinct keys.
- Ordinary guest stores in `core/memory.cpp` do not unconditionally broadcast
  `InvalidateCacheRange`. Rasterizer page-table marking and write notifications
  concern GPU coherency. Guest instruction-cache maintenance and kernel operations
  that expose/modify executable memory provide instruction-cache invalidation.
  `KPageTableBase` broadcasts relevant ranges to the process's core interfaces.
- Fastmem faults can also invalidate a block directly in `FastmemCallback`, outside
  `Jit::Impl::InvalidateCacheRange`. These now have separate counters. Removing
  invalidation or disabling self-modifying-code support would not fix capacity.

## Change and bounded expectations

The UWP A64 default is now 192 MiB/core; exact `NXBOX_JIT_LARGE_CACHE=0` restores
128 MiB for an A/B run. Other accuracy/optimization settings, executable-memory
permissions, block invalidation and the 8 MiB null JIT policy remain unchanged.
Empty invalidation ranges are ignored instead of allowing `length - 1` to wrap.

Four application-core code arenas are bounded to 768 MiB, +256 MiB versus the old
policy. Null JITs, metadata, history and other processes are additional allocations.
The captured process peaks at 4,688.20 MiB against 5,120 MiB, leaving 431.80 MiB
before the change. Extra code capacity alone fits that recorded headroom; retained
metadata also consumes memory. Arbitrarily adopting desktop's 2 GiB code budget
would be inappropriate here.

This removes the old 128 MiB ceiling, but **192 MiB is not a measured upper bound on
DBSZ's working set**. The host tests prove the threshold and invalidation behavior,
not a console frame-rate improvement or near-zero steady-state compilation. A
console rerun is necessary to establish whether this bounded increase is sufficient.

## Reading the next capture

All event counts below are window deltas, printed as `[count]` in `GAME_STALL`:

| Fields | Meaning |
| --- | --- |
| `jit_clear_capacity`, `jit_clear_explicit` | Applied full clears, partitioning `jit_clears`; coalesced requests count once |
| `jit_clear_req`, `jit_clear_blocks` | Explicit API requests and resident descriptors discarded by full clears |
| `jit_inv_calls`, `jit_inv_bytes`, `jit_range_batches` | Nonempty range requests/bytes and applied batches; a full clear can subsume pending ranges |
| `jit_range_blocks`, `jit_inv_empty` | Resident blocks removed and range applications with no matching descriptors |
| `jit_ic`, `jit_pt` | Guest instruction-cache operations and kernel page-table invalidation notifications |
| `jit_fastmem_faults`, `jit_fastmem_inv` | Fastmem callbacks and fault-driven requests to invalidate a descriptor |
| `jit_disk_stale` | Runtime disk-cache guards rejecting changed guest instructions; also submits a range request |
| `jit_sample64_new`, `jit_sample64_repeat`, `jit_sample64_unknown` | Exact counts for a stable 1/64 hash sample of complete descriptors, per JIT; history survives clears |
| `jit_compiled0` through `jit_compiled3` | Emissions per core, excluding disk restores |

The sampled history spans all guest addresses and FPCR variants, instead of just
startup. It holds at most 65,536 sampled keys per JIT; overflow is explicitly
unknown. Unsampled misses do not increment these three fields. Do not treat them
as exact whole-workload totals or multiply them into a precise unique-block count.

`jit_cacheN=[used/capacity]` remains a latest sample. `jit_cache_peakN=[bytes]`
preserves the lifetime high-water mark even after a clear. Slots are per core,
not sums across processes. Capacity flush timing now measures the actual clear,
instead of recording a zero-duration event, on both A64 and A32 x64 backends.

After the same scene settles, check that capacity clears stop, `jit_compiledN`
approaches zero and occupancy stays below capacity. If clears persist, the
capacity change is insufficient; if sampled-new keys keep rising without clears,
the game is still discovering code. Sampled repeats without full clears must be
correlated with range/fastmem/disk invalidation counters. Record process memory
alongside these fields before increasing the budget further.

## Host validation

`tests/port/test_jit_cache_reasons.py` compiles production A64/A32 invalidation
methods with host stubs, exercises capacity and explicit clears, coalesced range
requests and zero-length ranges, and runs with profiling both enabled and disabled.
It also checks window resets, zero-duration legacy flush counts, cache high-water
marks, per-core emission deltas and history samples beyond the startup prefix.
Existing hot-path tests exercise production descriptor unlinking and invalidation.
`test_dbsz_jit_capacity.py` executes the real policy with all environment cases.
These checks do not execute emitted Xbox code or run a guest game.

## Follow-up: 192 MiB capture (`s-dbsz`)

Evidence read on 2026-10-20:
`/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/s-dbsz/diag.txt`.
There are 38 `GAME_STALL` records. **This file does contain two full capacity
clears.** Comparing nonadjacent windows with zero *window-delta* clears hid the
intervening resets. No occupancy reporting bug is needed to explain this run.

| Zero-based window | Core | Occupancy before → after (bytes) | Clear blocks |
| --- | --- | --- | --- |
| 13 | 0 | 193,213,677 → 27,827,732 | 382,462 |
| 23 | 1 | 199,979,216 → 50,838,539 | 389,690 |

These are the **only adjacent-window occupancy decreases** in the capture.
Both have `jit_clears=[1]`, `jit_clear_capacity=[1]`, `jit_clear_explicit=[0]`,
and a timed `jitflush`/`jitinvalidate` (174 ms and 214 ms). The actual pre-clear
high-water marks are 200,278,031 and 200,278,651 bytes: the last published sample
before a window is not necessarily the last sample before a clear. The ~173 MB
sample is window 7; the later ~138 MB sample is window 34, after the core-0 clear.
192 MiB delayed eviction; it did not eliminate it.

### What the counts and occupancy mean

`GetBlock` first checks `emitter.GetBasicBlock(descriptor)`. A hit returns before
creating the `Kind::Jit` scope. Consequently the third field in
`jit=total_ms/max_ms/calls` counts **miss-handler invocations** that have finished,
not lookups, IR instructions, optimizer passes, guest instructions or executed
blocks. One invocation normally translates/optimizes/emits one basic block; a disk
restore can satisfy a miss without compilation. A descriptor includes PC, FPCR
state and single-step state. Normal links and fast dispatch can bypass this lookup.

`jit_compiledN` counts actual emissions in core N's JITs, excluding disk restores;
it increments after emission and profiling history updates. Its slots are shared
by all process interfaces with that processor ID. It is a window delta, whereas
`jit_cacheN` is the latest sampled code-arena cursor in that slot, including the
prelude, **not live descriptor sizes or a sum across process interfaces**.

This capture has 1,560,936 miss-handler calls, exactly 1,560,936 emitter calls,
and 1,560,936 per-core compilations (644,150 / 709,511 / 207,275 / 0). The small
count differences *within* individual windows come from separate atomic snapshots
and in-flight compiles, not hundreds of thousands of hidden optimization passes.

`BlockOfCode::ClearCache` rewinds the allocation cursor. Range invalidation removes
descriptors, dispatch entries and inbound links without reclaiming emitted bytes;
fastmem invalidation uses the same descriptor-removal machinery. `Patch` temporarily
moves the cursor to incoming sites and then restores it; it does not sample cache
occupancy while patching. Neither unlinking/relinking nor a page-table fault can
shrink this cursor permanently. A different process/interface can overwrite a core
slot with a different cache sample; that remains a limitation of these slots, but
there is no evidence that it explains this capture. All range requests/batches,
range-discarded blocks, instruction-cache operations, page-table notifications,
fastmem faults/invalidation and stale disk guards are zero here.

The stable sampled history has 20,778 first-seen descriptors, 3,677 repeats and
zero overflow. **Every sampled compile before window 13 is first-seen in its JIT.**
Repeat samples start with the first capacity eviction. Thus this run shows
substantial cold code discovery followed by capacity-induced recompilation, not a
write-triggered invalidation storm. First-seen per JIT does not prove that the game
streams/generated new guest instructions: core migration and FPCR variants also
produce first-seen descriptors. These data cannot establish a global unique-PC
working set or prove that a particular scene has reached steady state.

### Where the compilation time goes, and the change

Totals below sum the displayed, rounded milliseconds across cores and windows;
these are nested host elapsed scopes, not additive CPU percentages or exclusive
wall-clock stalls on the frame's critical path.

| Scope | Total | Share of inclusive JIT time |
| --- | ---: | ---: |
| `jit` | 51,648 ms | 100% |
| `jittranslate` | 3,423 ms | 6.6% |
| `jitoptimize` | 2,676 ms | 5.2% |
| `jitemit` | 41,617 ms | 80.6% |
| `jitemitinsts` (inside emit) | 29,287 ms | 56.7% |
| `jitemitterm` (inside emit) | 4,840 ms | 9.4% |
| `jitemitranges` (inside emit) | 2,178 ms | 4.2% |
| `jitflush` (inside JIT) | 388 ms | 0.8% |

For example, window 31 has 102,607 misses: `jit=3500`, translate=224,
optimize=171, emit=2882, and emit-instructions=2020 ms. These are real block
compiles, averaging ~34 microseconds inclusive per miss across the CPU cores.
Protection/commit timers and protected-byte counts are effectively zero; disk
capture is zero. Cheaper optimization passes alone target only ~5% of this cost.

The x64 emitter's register allocator did two unnecessary exhaustive scans:
`ValueLocation` visited up to all 102 register/flag/spill positions on each query
(including successful "not already defined" assertions); `EndOfAllocScope`
called `ReleaseAll` on all 102 positions after **every emitted IR instruction**.
Most such positions are empty. This work is inside `jitemitinsts`; the old timers
cannot separate its exact share from instruction encoding and the other emit work.

The allocator now maintains two small bit masks: live locations, and locations
modified since the previous IR instruction. Lookup checks only live or modified
candidates, retaining `ContainsValue` as the authority. Scope exit releases only
modified locations and updates the live index. Moves, swaps, spills, aliases,
partial releases and last-use reuse all go through the indexed mutable accessor.
No optimization pass, register-selection policy, block boundary, invalidation
semantics or emitted instruction sequence is intentionally changed. This reduces
per-block emission work for cold and recompiled blocks, rather than guessing at a
cheaper cold optimization tier or sharing code with embedded per-core context.

Both A64 and A32 also explicitly destroy the previous `RegAlloc` before its
placement reconstruction. Previously an alias vector that outgrew its inline
capacity could lose its heap allocation without destruction. This fixes a concrete
lifetime leak, not a measured explanation for the 2–4 second windows.

The default remains 192 MiB/core. Recorded peak process memory is 5,134,368,768 /
5,368,709,120 bytes, only 223.5 MiB headroom. Another uniform +64 MiB/core alone
would exceed that headroom, before retained metadata. The change does not claim to
stop all steady-state compilation or establish an Xbox FPS improvement; those
require another console run. The last window already falls to 156 compilations /
8 ms JIT, so a universal permanent compilation storm is not supported either.

### Additional counters and host validation

New `GAME_STALL` window counts:

- `jit_guest_code_reads`: A64 translator instruction-fetch attempts (including
  failed/retried fetches), excluding disk validation reads.
- `jit_ir_insts`: optimized A64 IR instructions passed to emission.
- `jit_host_bytes`: emitted A64 block bytes, including terminals/deferred code,
  excluding alignment gaps and prelude. These three fields distinguish block size
  and code expansion from an expensive individual compiler operation.
- `jit_ra_lookups`, `jit_ra_probes`, `jit_ra_releases`: allocator value queries,
  candidate positions examined and scope-release calls, aggregated locally and
  published once per emitted A64/A32 block; no per-IR atomic increment or clock.
  Compare probes/query and releases/IR instruction with the old upper bound of
  102 positions per query and exactly 102 releases per IR instruction.
- `jit_clear_totalN`: lifetime applied full clears observed in core N's profiling
  slot, **not reset at a reporting boundary**, so a later zero-clear window cannot
  hide an earlier eviction. Like other core slots, this can include multiple
  process interfaces. Existing clear-reason deltas remain available.

`tests/port/test_jit_reg_alloc.py` executes production allocator metadata methods
with host emission/IR/vector stubs. It checks the index against exhaustive lookup
through 20,000 randomized operations and compares scope-exit state with the old
exhaustive release loop, plus deterministic aliases, last-use reuse,
partial release, moves, exchanges and spills at locations 63/64/101; profiling is
compiled both on and off; an additional host ASan/UBSan run passes. It also
executes both production reconstruction snippets
with tracked vector lifetimes, including aliases beyond inline capacity.

`python3 tests/port/test_jit_reg_alloc.py --benchmark` optionally compares the old
exhaustive lookup/scope loop with the indexed methods on a sparse synthetic
workload (2 million operations). On this Mac, one run measured 254,929 → 9,314 us
with identical lookup results. This is a bookkeeping microbenchmark, **not an
emulator benchmark or an expected 27x frame-rate gain**. The current capture cannot
retroactively measure register-allocation time; use the new work counters and
`jitemitinsts`/`jit` timings on the same scene in the next run.

Final validation: `python3 -m unittest discover -s tests/port` ran 889 tests
in 410.465 s: 843 passed, 44 skipped, and two environmental import errors
(`test_button_layout` and `test_xbox_button_icons`, both missing `fontTools`).
The focused `*jit*.py` run passed 46 tests with two skips. The final allocator
regression passed with profiling on/off and separately under ASan/UBSan.
`git diff --check` passed. No Xbox rebuild or console performance rerun was made.
