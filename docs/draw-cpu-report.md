# Per-draw CPU work report

M1 is implemented in source. M2 has diagnostic instrumentation but cannot be
completed without new Xbox measurements. M3 was not started, following the
instruction to stop at an incomplete milestone. The 30% target is not demonstrated.
No Xbox build or rendering comparison was possible on this host. No commit or push
was made, and tools, workflows, keys, games, and saves were not modified.

## Skipped work and invalidation

| Cache | Exact inputs / work skipped | Invalidation |
| --- | --- | --- |
| Pipeline input reuse (new) | Maxwell dirty bit covering fixed PSO state and the existing shader-specialization key; exact comparisons of registers whose two dirty slots are occupied; refreshed guest shader hashes; draw-manager topology; actual color/depth formats and sample count. Skips fixed-state reconstruction, specialization hashing, feature checks, and guest-map lookup for a finished pipeline. | Context recording generation, format/sample changes, tracked register writes, collision-value changes, topology/hash changes, channel bind, shader removal, guest-cache mutation, disk-cache reload. Null/pending/failed results are never memoized. Shader synchronization and unsupported-state validation still run before reuse. |
| Attachment descriptions (new) | Retained resource identity; D3D12 resource descriptions are immutable. Reuses GetDesc results in validation, overlap, extent, and dimension calculations. Fixed capacity of nine resources, no hot-path allocation. | Context generation (including attachment descriptor rewrites); resource mismatch replaces an entry. Retained resources prevent identity reuse. View ranges and view formats are still evaluated each draw. |
| Fixed RTV/DSV descriptors (existing, strengthened) | Resource identities, mip/layer, view format, RT count, render layers, depth read-only flags. Skips CreateRtv/CreateDsv for identical inputs within a recording generation. | Any key change, or Context recording-generation change. A rewrite invalidates all recorded bindings before updating the generation snapshot. |
| Predication (new) | Native predicate resource pointer; offset is always zero and operation always EQUAL_ZERO. All query and transform-feedback writers use one Context setter. Repeated null pauses or identical active bindings skip SetPredication. | RecordedDrawState generation refresh: command-list reset/submit, target rewrite, and existing binding resets. Different predicate/null state records immediately. Commands() runs before consulting the cache, accounting for lazy command-list reset. Predicate resource transitions remain mandatory. |
| IA/RS/OM and root arguments (existing) | Exact API-value representations for topology, VB/IB views, viewport/scissor arrays, stencil refs, blend factor, depth bounds, RT handles/count, PSO/root signature, descriptor handles, CBV addresses and constants. Existing comparisons already skip redundant calls. | Context generation on command-list reset/submit, RTV/DSV rewrite, compute/clear/blit and existing binding resets. Heap/root-signature changes clear root arguments. Channel bind and pipeline-provider replacement now also invalidate Context state. |

The common dirty-table entries are preserved: a new pipeline bit occupies an
unused slot; collisions compare the register values directly because shared caches
may consume their own dirty bits before pipeline lookup. The dependency-coverage
test checks all register roots used by FillGraphicsState and specialization.

Both UpdateRenderTargets calls remain. Their PrepareImageView work synchronizes
aliases and marks images modified; the second call is needed after sampled-texture
synchronization. Skipping that work based only on unchanged RT registers would
change rendering. Native BeginQuery/EndQuery/ResolveQueryData remain per draw;
identical query-enable state does not imply identical counter results.

## What between_draws measures

FrameTiming starts in BetweenDraws and RAII phase scopes restore their previous
phase. This is the residual recording-thread time outside all instrumented phases,
not a dedicated rasterizer operation. Although named *_cpu_ms, the phase clock is
QueryPerformanceCounter, so these are exclusive wall times, potentially including
scheduling or idle time. gpu_thread_cpu_pct separately uses GetThreadTimes.

The supplied profile contains 890.742 ms / 205134 draws = 4.342 us/draw of residual
time, but no breakdown from which to rank its contributors. Guest commands,
uniform-binding callbacks, guest query reports, cache maintenance, fence callbacks,
and frame maintenance are candidates, not measured winners.

Enable both flags for the next Xbox capture:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_D3D12_BETWEEN_DRAWS_PROFILE=1
```

The new flag defaults OFF. Every existing five-second profile window also emits
D3D12_BETWEEN_DRAWS_PROFILE top_wall_ms with categories sorted by exclusive elapsed
time: guest_queries, uniform_bindings, cache_maintenance, fences, frame_maintenance,
and unattributed_guest_or_idle. Nested upload, submit, barrier, draw, and wait phases
are excluded; nested callback categories restore their parent. The categories sum
to the original between_draws phase. Unattributed time still needs a sampler if it
dominates; this instrumentation does not invent a breakdown inside the guest
command interpreter. No between-draw optimization was selected without results.

## Incremental estimates, not measurements

| Phase | Supplied baseline (us/draw) | Estimated saving (us/draw) | Limit / risk |
| --- | ---: | ---: | --- |
| pipeline_lookup | 1.364 | 0.4–0.9 on repeated inputs | Dirty rate determines hits; unsupported-state validation, shader synchronization, and the mutex remain. Conservative ranges can cause extra misses. |
| attachment_setup | 1.485 | 0–0.25 | Only immutable native description fetches are reused. Validation/range calculations remain. A very cheap GetDesc implementation could erase the gain. |
| query_state | 1.143 | 0.02–0.12 | Only redundant predication state is skipped; native query work remains. Active conditional rendering still requires pause/resume. |
| draw_dispatch / draw_cleanup | — / 1.145 | 0.02–0.1 combined | Additional redundant null-predication pauses are skipped. No allocation/cleanup optimization yet. |
| graphics_state / render_targets | 0.484 / 0.541 | Approximately 0 additional | Existing IA/RS/OM and fixed-slot reuse already captured most of these wins. Extra invalidation can add a small rebind cost after resets. |
| between_draws | 4.342 | No claimed saving | New callback scopes add a disabled-flag check/call; enabled diagnostics add timestamps and window logging. |
| descriptor_setup | 2.336 | 0 | M3 deferred. Previous-draw table reuse and its counter are unchanged. |

A rough combined expectation is 0.4–1.3 us/draw with enough repeated state, around
2–6% of the supplied 21 us/draw. This is not a promise of a 30% improvement, nor a
claim that output equivalence has been verified on hardware. Equivalence follows
from immutable descriptions and exact input/state matching; the main maintenance
risk is a future pipeline-key input being added without dirty tracking.

## Host validation

`python3 -m unittest discover -s tests/port -q` completed with exit code 0:

```text
Ran 520 tests in 161.411s

OK (skipped=39)
```

The exact elapsed time is recorded in /tmp/draw-cpu-tests-final.log. The skipped
cases remain environment-dependent; this result does not compile the Windows/Xbox
renderer. `git diff --check` also passed.

The existing pinned tests updated were test_d3d12_bound_targets.py and
test_d3d12_redundant_state.py (the same attachment-change guard now also checks the
recording generation), and test_d3d12_pso_lookup_fast.py (reuse now precedes key
construction, while the old exact-request fallback remains). test_t8_perf.py gains
an executable check that detailed residual timing excludes nested phases, sums to
BetweenDraws, and clears at reset. test_d3d12_draw_cpu.py guards key-input coverage,
recording-generation checks, and centralization of every native predication writer.
test_d3d12_guest_visibility.py updates its extracted-method host mocks for the
optional timing scope, reuses the real FrameTiming type, and exercises both normal
profile output and the opt-in detail output. Its cache-visibility assertions remain
unchanged. C++ formatting was applied to changed line ranges with the repository
style; the touched Python tests were checked with ruff format.
