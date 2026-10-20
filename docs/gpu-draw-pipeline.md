# Optional two-stage D3D12 draw pipeline

Enable with `NXBOX_GPU_PIPELINE=1`. The switch is read through `BisectEnabled`
when the D3D12 rasterizer is created. Default is **off**. Restart the game/session
when changing it. Other renderers retain their existing path.

## Evidence and intended cut

The supplied BotW profile describes 72.82 ms/frame of exclusive GPU-thread
intervals within a 73.53 ms present period. DMA pushing is 6.01 ms, Maxwell
methods 8.27 ms, and macro execution 2.12 ms: **16.40 ms/frame** of command
frontend work. Host draw building includes recording 7.33 ms, descriptors
7.25 ms, buffer binding 6.05 ms, and texture updates 5.58 ms:
**26.21 ms/frame** in those four buckets alone. The previous producer spends
86.5% of a host core, with no recorder backpressure. These are elapsed phase
intervals, including waits, rather than independent measurements of CPU cycles.

The source is the supplied `botw-vertex-binding-prefix.md` analysis. Re-running
`perfcmp.py` against the four supplied captures gives:

| Capture | Gameplay windows | Median fps | Worst gap | Peak MiB |
| --- | ---: | ---: | ---: | ---: |
| x-botw-rec | 37 | 12.9 | 792 ms | 4061 |
| x-ody-rec | 29 | 24.7 | 1359 ms | 3709 |
| x-mk8d-rec | 26 | 31.0 | 1386 ms | 3744 |
| dbsz-final105 | 39 | 18.8 | 52513 ms | 4594 |

DBSZ uses `eden_uwp_diag.txt`. These captures precede this implementation and
are not an A/B. There is no measured improvement from this change yet.

The cut permits command decoding for later draws to overlap host building for
earlier draws. It does not remove either cost. Even ideal overlap of the full
16.40 ms frontend would leave roughly 56 ms of the original intervals in the
other lane, before considering queue overhead and ordering stalls. That is an
illustration of the budget, not a prediction. The supplied profile also has
28.52 ms/frame of physical GPU timestamp intervals on Series X. This change
alone cannot establish a 16.67 ms budget or locked 60 fps on Series S.

## Boundary and ownership

1. The existing GPU thread runs the DMA pusher, Maxwell method/register
   processing, draw merging, condition evaluation and macros.
2. Direct `Draw` captures an owned snapshot and publishes it into a FIFO with
   capacity **64**, including the currently executing job. A full FIFO blocks
   publication. The new `GPU draw builder` thread applies the snapshot to a
   private Maxwell engine and runs the existing D3D12 draw implementation.
3. The existing D3D12 command recorder remains a separate, later stage. Its
   command encoding and native command-list replay are unchanged.

A snapshot owns the complete Maxwell register file, shader-stage constant-buffer
metadata, draw manager (including inline indices), already-computed dirty flags,
HLE replacement table, and HLE engine hint. Full registers deliberately preserve
the existing rasterizer, shader environment, buffer-cache and texture-cache
interfaces. This costs a register-file copy per draw; it is not a compact delta
encoding. Inline indices and replacement tables have variable-size storage, so
the bound is on outstanding snapshots/jobs rather than a fixed byte budget.

Only the decoder writes the frontend engine. Cache channel registrations point
to the channel's private renderer engine when this mode is enabled. They never
consume dirty flags from the decoder. Capture clears the decoder's flags;
application ORs flags into the consumer's remaining flags, preserving deferred
or cache-generated invalidations. Dirty lookup tables and macro execution
machinery are initialized once, rather than copied per draw.

Synchronous rasterizer entrypoints use the same worker and wait for their FIFO
job to finish. This includes cache callbacks, uniform-buffer binding, queries,
fences, barriers, flushes, clears, compute and copies. When called by the GPU
producer, these jobs also carry the current frontend snapshot. A thread-local
producer marker prevents CPU invalidation callbacks from reading live frontend
registers. Nested calls on the consumer execute inline and cannot wait on their
own queue. Reference-returning entrypoints preserve reference identity.

Presentation/composition also runs synchronously on this worker, so runtime
command construction and frame-profile accounting retain one owner. Channel
initialization/binding/release are synchronous; a channel change drains the old
channel before rebinding cache references. Direct draws service guest sync
requests on the producer after publication, rather than concurrently inside the
consumer draw implementation.

The worker registers with the core, uses performance cores, and receives a thread
name and critical priority. GPU-thread shutdown joins it before channel payloads
can be destroyed, even after a queue failure. Rasterizer teardown also closes
and joins before clearing the queue pointer or destroying caches/context. Close
is idempotent, drains accepted work and rejects later publication. Asynchronous
job exceptions cancel queued jobs and wake blocked publishers/drainers;
synchronous job exceptions propagate through their futures.

## Ordering and guest memory

`MemoryManager` drains outstanding draw building before exposing guest pointers
or spans, block reads/writes (including unsafe/cached writes), cache-accumulator
flushes, and map/unmap operations. The typed pointer helpers also use the barrier.
The enable flag is latched when the rasterizer binds, avoiding a virtual barrier
call in the default disabled path. Consumer-side accesses bypass the wait.

DMA processing drains before reading each new segment and copies the segment
into an owned command-word vector before decoding. Existing memory-sync segment
boundaries still apply. Subsequent decoding reads that copy while the consumer
may execute earlier draws. CB uploads cannot mutate bytes under an older draw:
the upload's memory barrier completes that draw first. The next draw flushes
the existing invalidation accumulator through the unchanged cache path.

Puller semaphore reads use the memory barrier, while signals/fences and query
reporting use synchronous worker calls. Query conditions therefore cannot read
before older host draws have been built; their existing native GPU/query waits
remain responsible for actual GPU completion. A queue drain alone is **not** a
hardware fence. Fermi2D, Kepler compute/copies, Maxwell DMA and inline uploads
retain their existing synchronous memory/native ordering after that drain.

As with the previous renderer, games must synchronize CPU/guest writes against
GPU use. Snapshots do not copy arbitrary vertex, texture, shader or CB memory and
do not make unsynchronized guest memory mutations well-defined.

## What is not yet pipelined

- Indirect draw expansion, textured blits, clears and compute are synchronous.
- Constant-buffer binding, guest-memory reads/writes, uploads, queries,
  semaphores, barriers, copies, submissions, channel changes and presentation
  interrupt producer/consumer overlap.
- Macro code runs on the producer; macro memory reads drain. Only direct draws
  emitted by macros are queued.
- The host draw-building lane is still serial. Descriptor, buffer and texture
  work are not independently parallelized or replaced.
- There is no resource-memory snapshot, compact register delta, byte-budgeted
  queue, adaptive batching, or per-game enablement policy.

Conservative memory and API barriers may leave very little overlap in a real
game. Snapshot allocation/copying and synchronous marshaling may regress CPU
cost. Measure before changing the default.

## Validation and next console run

`test_gpu_pipeline.py` compiles host tests with clang++, warnings as errors,
`-Wshadow`, ASan and UBSan. It replays persistent controlled method recordings
through serial and queued paths with capacities 1, 2 and 64. One replay compiles
the production `DrawManager` declaration, dispatch and state transitions with
hardware stubs: **1800 matching draws per capacity**. It covers arrays, indexed
and inline-index draws, instance merging, topology changes and skipped draws.
The queue contract replay produces **2400 matching draws per capacity**, with
explicit synchronous ordering points and mutable frontend state.

Further checks exercise snapshot ownership, HLE hint capture, dirty-flag union,
owned DMA words, queue saturation, consumer reentrancy, result/reference returns,
exceptions, rejected publication, close/join, and profiling TLS isolation. A
compiled extraction of the production synchronous dispatcher checks that CPU
callbacks do not snapshot the live producer and that nested worker calls stay
inline. Source integration checks cover every rasterizer override and memory
barrier. These recordings are controlled fixtures, **not a full captured BotW
method stream**. D3D12 hardware, caches and shader execution remain mocked.

The initial new suite failed before implementation (`failures=1, errors=1`).
See the final test-results table below for completed host validation.

For a console A/B, use the same route with `NXBOX_GPU_PIPELINE=0` and `=1`, both
`NXBOX_D3D12_FRAME_PROFILE=1` and `NXBOX_THREAD_PROFILE=1`. Compare fps, visual
output, draw counts, worker utilization, memory, query correctness and shutdown.
Test BotW, SMO, MK8D and DBSZ, then repeat without instrumentation.

With the pipeline enabled, D3D12 frame phase timing and `gpu_thread_cpu_pct`
belong to **GPU draw builder**. The producer does not share that sink; its
DMA/Maxwell/macro phase fields are not a measure of the now-parallel frontend.
`NXBOX_THREAD_PROFILE` can identify utilization by the new thread name. The
existing `NXBOX_SAMPLER` targets the original GPU/guest threads and does not
sample the new builder. Do not interpret missing frontend phase time as work
eliminated, or add timing from parallel lanes as serial frame cost.

Not verified here: MSVC/UWP compilation and its warning policy, real D3D12
rendering, a full game method capture, console shutdown/device loss, measured
latency/fps improvement, or Series S behavior and 60 fps.

## Completed host checks

All unittest commands use `python3 -B -m unittest discover -s tests/port -p`.

| Pattern / command | Final output |
| --- | --- |
| `test_gpu_pipeline.py` | `Ran 4 tests ... OK`; 5400 production draws and 7200 queue replay draws matched |
| `test_gpu_frontend_hot_paths.py` | `Ran 9 tests ... OK`; 800 DMA seeds / 145592 segments; 480000 sink-oracle checks |
| `test_d3d12_*.py` | `Ran 265 tests in 96.913s ... OK (skipped=3)` |
| `test_d3d12_draw_phases.py` (after final sync-request cadence adjustment) | `Ran 2 tests ... OK` |
| `test_d3d12_vertex_binding_prefix.py` (after final adjustment) | `Ran 2 tests ... OK`; `6000 IA replay draws OK` |
| Native queue replay compiled with `-fsanitize=thread` | All three capacities matched; snapshot, saturation, shutdown, cancellation and TLS checks passed |
| Ruff format/check on new Python test; clang-format on edited C++ ranges/new C++ files | Completed; `All checks passed!` |
| `git diff --check` | Exit 0, no output |

The broad D3D12 run needs
`PKG_CONFIG_PATH=/opt/homebrew/opt/fmt/lib/pkgconfig` on this host. Its first run
reported an unavailable `fmt.pc` and one stale source assertion that still
expected caches to bind the frontend engine. The assertion now requires binding
the selected engine after `BindToChannel`; disabled-mode cache mocks were also
extended with the new dispatcher interface. The three skipped optional tests
remain unverified.

An additional cancellation regression failed before its fix: a queued
synchronous future stayed blocked after an older asynchronous job failed. Moving
the packaged task's only owning reference into the queue closure makes cancelling
that closure break the promise and wake the caller. That regression now passes
under ASan/UBSan and ThreadSanitizer.

ThreadSanitizer invocation:

```sh
clang++ -std=c++20 -O1 -g -Wall -Wextra -Wshadow -Werror -pthread \
  -fsanitize=thread -Isrc tests/port/gpu_pipeline_test.cpp \
  -o /tmp/nxbox-pipeline-tsan
/tmp/nxbox-pipeline-tsan tests/port/fixtures/gpu_pipeline/methods.txt
```

## Files changed

- `src/video_core/gpu_draw_pipeline.h`: bounded FIFO, owned snapshots and DMA words.
- `src/video_core/control/channel_state.h`, `channel_state_cache.cpp`: isolated renderer engine.
- `src/video_core/dma_pusher.cpp`: own command segments while overlapping draws.
- `src/video_core/gpu.h`, `gpu.cpp`: register the renderer worker with the core.
- `src/video_core/memory_manager.h`, `memory_manager.cpp`,
  `rasterizer_interface.h`: guest-memory ordering boundary and disabled-path gate.
- `src/video_core/renderer_d3d12/d3d12_rasterizer.h`, `d3d12_rasterizer.cpp`,
  `renderer_d3d12.cpp`: optional draw publication, synchronous dispatch,
  composition and shutdown.
- `src/common/gpu_thread_profile.h`: keep the recording gate local to its owning thread.
- `tests/port/test_gpu_pipeline.py`, `gpu_pipeline_test.cpp`,
  `fixtures/gpu_pipeline/{methods.txt,maxwell_methods.txt,maxwell_replay.cpp}`:
  queue/dispatcher contracts and production DrawManager replays.
- `tests/port/fixtures/gpu_frontend_dma.cpp`,
  `test_d3d12_guest_visibility.py`, `test_d3d12_state_tracker.py`: update existing
  hardware-free mocks and the selected-engine binding assertion.
- `docs/gpu-draw-pipeline.md`: evidence, ownership/ordering design, remaining
  synchronous work and validation limitations.

## Commit status

Current branch: `p8/pipe`. The requested commit could not be created: `git add`
failed with exit 128, `Unable to create .../.git/worktrees/wt-p8-pipe/index.lock:
Operation not permitted`. The worktree's Git metadata is outside the sandbox's
writable roots. No files were staged by this attempt, no commit was created and
nothing was pushed. Intended commit identity is Miguel <miguel@nspx.dev>, with
message `Add optional two-stage D3D12 draw pipeline`.
