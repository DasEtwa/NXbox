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
