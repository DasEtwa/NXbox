# Wonder first-use shader stalls

## Evidence

Analyzed the existing `compat-2026-10-18` artifacts on 2026-10-19. There are no
`x-wonder*` or `y-wonder*` directories. Used `w1`, `w1b`, `w2`, `prof-wonder`,
`samp-wonder`, `r105-wonder` and `rc-wonder` instead. These are historical runs,
not measurements of this patch.

| Run | Gameplay median | Worst presentation gap | Explicit PSO waits |
| --- | ---: | ---: | ---: |
| `w1` | 26.4 fps | 9,490 ms | 124; 70,744 ms total; 3,704 ms maximum |
| `w1b` | 23.6 fps | 4,314 ms | 794; 36,409 ms total; 4,254 ms maximum |
| `w2` | 28.6 fps | 1,269 ms | 2,641; 2,917 ms total; 51.8 ms maximum |
| `prof-wonder` | 27.7 fps | 639 ms in the gameplay segment | PSO wait tracing absent |

`perfcmp.py` selects gameplay after the last menu window. Different routes,
cache contents and settings prevent treating these runs as a controlled A/B.
The entire `prof-wonder` run has an 881 ms maximum gap, including loading.

For completed graphics jobs in `w1`, consecutive `PSO_TRACE` phase timestamps
attribute 53,841 ms to DXC, 13,872 ms to native driver PSO creation, 462 ms to
translation, 387 ms to emission and 194 ms to root signatures. These are sums
across worker jobs and can overlap in wall time. DXC is the largest compile
consumer. The renderer already uses independent, thread-local DXC caches;
there is no shared DXC compilation mutex to remove.

The GPU thread nevertheless waits up to **5,000 ms per new protected request**.
Multiple new requests in one frame multiply that timeout. A per-request 16 ms
timeout would still allow a large hitch, so the new policy shares one deadline.

## Gameplay pacing

`Present(1, 0)` requests one refresh, not two. The VI conductor uses a 60 Hz
base; the hardware composer returns an interval of one and advances one frame.
The single-core speed limiter exits immediately in multicore mode. No fixed
30 fps host presentation limit was found, so presentation behavior is unchanged.

For `prof-wonder` at +80 to +140 seconds, `tprof.py` reports:

- 27.8 fps median; GPU thread CPU usage 75.8% of one host core.
- Present: 23.4 ms per five-second window, **0.17 ms per call**.
- Fence host waits: 806.5 ms per window; PSO compiler CPU median zero.

The final 12 frame-profile windows report weighted means of 8.33 ms native GPU
time, 5.71 ms Maxwell methods, 3.65 ms sync wait, 3.17 ms draw recording,
2.94 ms fence wait, 2.36 ms texture-cache update, 2.14 ms submission, 2.11 ms
buffer-cache binding and 1.48 ms pipeline lookup per frame. DXC and shader
translation are zero. CPU phase scopes can nest; these costs must not be added.
The other requested recordings also have zero warm DXC time: native GPU time
is 28.09 ms for `x-botw-rec`, 11.73 ms for `x-ody-rec` and 9.48 ms for
`x-mk8d-rec` in their final 12 profile windows. `dbsz-final105` has no usable
frame-profile rows for this calculation.

This supports a CPU/synchronization bottleneck in warm Wonder gameplay rather
than a Present wait or continuing shader compilation. It does not identify every
CPU stall or establish a single cause for all 30 fps scenes. Removing compile
waits alone does not establish a locked 60 fps, particularly on Series S.

## Changes and switches

- `NXBOX_D3D12_PSO_FRAME_BUDGET=1`: with async PSOs enabled (the Xbox default),
  graphics and compute first-use waits share a 16 ms deadline, reset at
  `RasterizerD3D12::TickFrame`. `NXBOX_D3D12_PSO_WAIT_MS` can shorten this wait
  but cannot extend it. Subsequent requests see only the remaining time.
- Existing `CanSkipPendingGraphicsPso`, stream-output protection and
  `NXBOX_D3D12_PSO_WAIT_RT_HISTORY` decisions remain intact. Eligible draws
  skip immediately; protected requests are urgently queued and get their one
  bounded wait. A timeout uses the existing null-pipeline skip path; later
  requests look up the same async entry until it is ready.
- In this mode compute translation and the global-memory fallback check run
  on the worker, before HLSL emission/DXC. Unsupported fallback requests still
  settle as failures rather than being translated repeatedly on the GPU thread.
- `NXBOX_D3D12_PSO_PARALLEL_DXC=1`: three runtime workers instead of two.
  Prewarm restores the selected runtime target on completion, cancellation,
  skip-wait and failure. Each worker retains its own DXC instance/cache.

Both switches use `BisectEnabled` and default **off**. Shorter protected waits
can permanently lose one-shot render/compute outputs; a third worker increases
memory and CPU contention. No visual correctness or memory evidence justifies
enabling either by default yet. Existing headroom admission remains in place.

The deadline bounds requested compilation waits, not OS scheduling latency,
environment snapshots, unrelated GPU work, or built-in helper pipelines that
use synchronous `GetGraphics`. Explicitly disabling async PSOs retains the
synchronous compilation path. There is no claim that every GPU-thread operation
now has a hard real-time bound.

## Validation

The three new port tests failed before implementation (`FAILED (failures=3)`),
then passed. They exercise deterministic deadline exhaustion/reset, a shorter
configured timeout, three simultaneously blocked workers, pending-to-ready
publication, and graphics/compute/frame-boundary wiring. The blocked-worker
case takes a shared 16 ms timeout instead of three independent five-second waits;
its 500 ms assertion allows host scheduler noise.

All commands use `python3 -B -m unittest discover -s tests/port -p PATTERN`:

| Pattern | Output |
| --- | --- |
| `test_d3d12_pso_*.py` | `Ran 15 tests` / `OK (skipped=1)` |
| `test_d3d12_async_pso.py` | `Ran 2 tests` / `OK` |
| `test_d3d12_compile_policy.py` | `Ran 2 tests` / `OK` |
| `test_d3d12_prewarm_threads.py` | `Ran 2 tests` / `OK` |
| `test_d3d12_tick_frame.py` | `Ran 1 test` / `OK` |
| `test_d3d12_backend.py` | `Ran 2 tests` / `OK` |

The optional census test requires `NXBOX_PSO_CENSUS_DIR` and was skipped.
Clang-format was applied only to changed C++ ranges and the new header; Ruff
reported `2 files left unchanged`. `git diff --check` passed.

Not verified: MSVC/UWP build and its warning policy, real DXC concurrency or
driver behavior on Xbox, cold-cache visuals (especially persistent render
targets/stream output/compute), new worker peak memory, Series S frame times,
or a console before/after performance comparison. The host worker test does
not execute DXC or D3D12.
