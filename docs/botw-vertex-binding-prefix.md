# BotW GPU-thread profile and vertex binding prefix

## Evidence and frame budget

Analysis date: 2026-10-20. Input directory:
`/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18`.
The current `z-botw/diag.txt` has a gameplay median of 13.6 fps.
For the approximately 73 ms frame, select `GAME_PRESENT` windows with
`12 <= game_fps <= 15`: 25 corresponding D3D12 profile windows, 1,773 profiled
frames. Mean per-window present period is 73.53 ms (median 73.65 ms).
Values below average each selected profile window's phase total divided by its
profiled frame count. Thread and wait reporting use independent approximately
five-second intervals, so cross-thread attribution is approximate.

**The emulator GPU thread is the main producer bottleneck.** It uses 86.5% of one
host core, has no recorder backpressure, and spends only 3.56 ms/frame waiting
for new queue work. Guest nvdrv event waits average 39.46 ms/frame. Submitted
output fences spend 68.03 ms before GPU-thread processing and 2.01 ms between
processing and completion; the latter includes host submission/fencing overhead,
not just hardware execution. This points primarily to the guest waiting for the
graphics producer, rather than the graphics producer waiting for guest CPU work.

Despite their `_cpu_ms` names, phase fields use `ProfileWallTime()` in
`FrameProfile::Switch`; they are exclusive elapsed intervals on the GPU thread,
including any waits or descheduling inside each interval. `THREAD_PROFILE`
and `gpu_thread_cpu_pct` measure actual host CPU time separately.

| Rank | GPU-thread exclusive phase | ms/frame |
| --- | --- | ---: |
| 1 | Maxwell methods | 8.27 |
| 2 | Draw recording | 7.33 |
| 3 | Descriptor setup | 7.25 |
| 4 | Buffer-cache bind | 6.05 |
| 5 | DMA pusher | 6.01 |
| 6 | Texture-cache update | 5.58 |
| 7 | Sync wait | 3.70 |
| 8 | Pipeline lookup | 2.92 |
| 9 | Attachment setup | 2.56 |
| 10 | Macro execution | 2.12 |
| 11 | Draw setup | 2.10 |
| 12 | Index flush caching | 2.09 |
| 13 | Query state | 2.06 |
| 14 | Resource bindings | 2.06 |
| 15 | Geometry setup | 1.54 |
| 16 | Draw cleanup | 1.50 |
| 17 | Submit | 1.07 |
| 18 | Draw dispatch | 0.84 |
| 19 | Index conversion | 0.63 |
| 20 | Buffer validation | 0.62 |
| 21 | Graphics state | 0.57 |
| 22 | Feedback setup | 0.56 |
| 23 | Barriers | 0.51 |
| 24 | Feedback copies | 0.50 |
| 25 | Descriptor writes | 0.45 |
| 26 | Remaining small phases | 3.93 |
| | All measured exclusive phases | **72.82** |
| | Difference from present period | **0.71** |

The difference is not a separately measured phase: profile/present boundaries,
independent counters and averaging prevent exact accounting.

Parallel lanes and waits must **not** be added to the phase table:

| Lane or wait | Measurement |
| --- | --- |
| Emulator GPU thread actual CPU | ~63.6 ms/frame, 86.5% of a core |
| CPUCore_2 | ~32.8 ms CPU/frame, 44.6% of a core |
| CPUCore_1 | ~24.1 ms CPU/frame, 32.8% of a core |
| CPUCore_0 | ~21.0 ms CPU/frame, 28.5% of a core |
| CPUCore_3 | Effectively idle |
| D3D12 recorder | 15.69 ms CPU/frame, ~21.2% of a core |
| Physical GPU, command-list timestamp intervals | 28.52 ms/frame |
| Guest nvdrv event waits | 39.46 ms/frame |
| Guest fence completion waits | 8.07 ms/frame |
| GPU queue idle wait | 3.56 ms/frame |
| GPU-thread explicit native fence wait | 0.20 ms/frame |
| Recorder backpressure | 0.00 ms/frame |
| JIT compilation, summed stall bucket | 2.87 ms/frame mean; 0.52 median; 33.81 worst window average |

JIT compilation is included in guest CPU activity, not an extra serial frame
phase. Guest execution/HLE cannot be separated further from these logs.
JIT page-table fallback callbacks average approximately 125k/s on core 0,
146k/s on core 2, and 209k/s on core 1. Counts do not measure their CPU cost.
The fresh capture contains no `SAMPLER` stacks, so no leaf-function CPU ranking
is inferred from it.

Sampled Maxwell method estimates are inclusive and must not be added to the
exclusive table: dirty flags 5.63 ms/frame, draw state 2.04, constant buffers
0.43, query/report 4.37, and draw dispatch 16.58. Nested draw/renderer work and
wall-clock waits explain why these exceed the exclusive Maxwell phase.

The scene averages 3,292 draws/frame, 15,311 root CBV binds/frame, 967 descriptor
copy calls/frame, 1,739 barriers/frame, and 36.85 command-list executions/frame.
No PSO/DXC compilation occurs in the selected D3D12 profile windows.

## Chosen cut

The draw path previously built 32 IA vertex views, fetched the same zero-buffer
GPU address 32 times, compared/copied the complete array in recorded state, and
serialized all 32 views when bindings changed. The native input layout only
consumes attributes where `enabled && dxgi`, with IA slot equal to the **attribute
index**, not the guest vertex-stream index.

`VertexBufferBindingCount` now finds the last native input-layout attribute.
The draw builds and records only that prefix, including all holes. Missing guest
attributes (`enabled == 2`) remain backed by the stride-zero buffer. A wider
subsequent layout rebuilds and binds its entire prefix; the recorded-array cache
already includes the count in its comparison. Unconsumed trailing native slots
may retain their previous values without affecting shader reads.

The zero-buffer address is fetched once per draw, removing 31 redundant COM
queries: approximately **102,037 calls/frame** at the captured draw rate. For a
synthetic four-input draw, the production `EncodeCall` layout shrinks from
**576 to 128 bytes**, including headers, alignment and snapshot padding. This
is a size/work reduction, not a measured BotW frame-time improvement. The logs
do not expose an input-layout-width distribution, and the whole 7.33 ms draw
recording phase cannot be claimed as recoverable.

This is a bounded cut in the second-largest exclusive phase plus geometry setup.
The largest Maxwell bucket mixes parser/state handling and profiling overhead;
its sampled estimates do not isolate a larger safe semantic shortcut. Existing
texture/descriptor-cache opt-ins remain unchanged. No files under
`src/video_core/buffer_cache` were changed.

Default: enabled, because the native input layout fully determines which IA
slots can be read. Bisect with `NXBOX_D3D12_NO_VERTEX_BINDING_PREFIX=1` to restore
32-view binding. The address-query hoist is unconditional and uses the same
immutable resource. Draw tracing retains all 32 views so existing diagnostics
also report the same unused-slot values. The switch is latched through
`BisectEnabled`, like the other draw switches.

## Other captures

Using the supplied `perfcmp.py` gameplay selection (after the last >=45 fps
window; unlike the steady-scene selection above):

| Capture | Windows | Median game fps | p10 | Worst gap | Peak MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| z-botw | 35 | 13.6 | 12.5 | 819 ms | 4,044 |
| x-botw-rec | 37 | 12.9 | 11.9 | 792 ms | 4,061 |
| x-ody-rec | 29 | 24.7 | 23.4 | 1,359 ms | 3,709 |
| x-mk8d-rec | 26 | 31.0 | 27.4 | 1,386 ms | 3,744 |
| dbsz-final105 | 39 | 18.8 | 0.9 | 52,513 ms | 4,594 |

DBSZ's file is `eden_uwp_diag.txt`, not `diag.txt`. These are different games
and routes, not a controlled A/B for this patch. `winstat.py` uses a different
window-selection policy for some games, so its window counts can differ.

## Validation and remaining work

Before the implementation, the new suite reported
`FAILED (failures=1, errors=1)`: no prefix integration or count helper existed.
Afterwards:

| Command (`python3 -B -m unittest discover -s tests/port -p ...`) | Output |
| --- | --- |
| `test_d3d12_vertex_binding_prefix.py` | 2 tests, OK; `6000 IA replay draws OK; 4-slot encoded bytes=128 legacy=576` |
| `test_d3d12_draw_contract.py` | 1 test, OK |
| `test_d3d12_draw_trace.py` | 6 tests, OK |
| `test_d3d12_recorded_reset.py` | 2 tests, OK; `9999 draw-state replays matched full reset; payload preserved` |
| `test_d3d12_root_binding_batch.py` | 2 tests, OK; `47394 individual binds matched 945 batches; empty and abandoned batches verified` |
| `test_d3d12_pso_normalize.py` | 2 tests, OK (skipped=1); `50000 random states: effective descriptors unchanged; native build sharing OK` |

The new native test uses clang++ with ASan/UBSan, warnings as errors and
`-Wshadow`; it exercises the production PSO helper, recorded-array cache,
command encoding and cross-chunk asynchronous replay. It compares every consumed
IA slot against a 32-slot reference across 6,000 draws, count changes, holes,
zero inputs, slot 31 and command-list invalidations. Python source integration
checks tie the production loop, count, trace fallback and bisect to that helper.
The normalization suite's real-cache corpus test is skipped without its optional
external cache fixture.

Formatting: clang-format on changed C++ ranges, Ruff on the new Python test.
`git diff --check` passes.

Not verified: MSVC/UWP compilation, actual console rendering, before/after fps,
Series S behavior, or a locked 60 fps. Next run: repeat the same BotW route with
both profiling switches, default binding prefix versus
`NXBOX_D3D12_NO_VERTEX_BINDING_PREFIX=1`; compare geometry setup, draw recording,
recorder CPU and game fps, and visually check BotW/SMO/MK8D/DBSZ. A separate
unprofiled A/B is needed to exclude instrumentation overhead. The existing
28.52 ms hardware timestamp budget on Series X already exceeds 16.67 ms; this
CPU-side cut alone cannot establish the Series S 60 fps target, and GPU cost
cannot be extrapolated by simply multiplying by three.

Commit attempt: `git add` was denied before staging because this worktree's Git
metadata is in the main repository outside the sandbox's writable roots:
`.git/worktrees/wt-p7-botw2/index.lock` (`Operation not permitted`). No commit
was created and nothing was pushed. The four changed files remain in the current
worktree on branch `p7/botw2`.
