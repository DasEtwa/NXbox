# Naruto host memory pressure on Xbox

Evidence read directly from `elzar-2026-10-18/{eden_uwp_diag.txt,nxbox_crash.txt}`
and `elzar-2026-10-19/{eden_uwp_diag.txt,nxbox_crash.txt}` under
`/Users/miguel/.local/share/nxbox/artifacts/`. The October 19 diagnostic contains
all 36,552 lines of the October 18 diagnostic as its prefix; these are not
independent reproductions. Line numbers below refer to the October 19 diagnostic.
All memory figures are MiB (bytes / 1,048,576).

## What the logs establish

| Session | Before load | After load | Loaded | First process sample | Peak process sample | Peak accounted D3D12 resources |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Connections, session beginning at line 7446 | 103 | 982 | 986 | 4373.04 | 4709.69 | 156.25 |
| Storm 4, cold cache, line 19863 | 91 | 2089 | 2089 | 4360.77 | 5119.33 | 582.88 |
| Storm 4, 339 cached pipelines, line 25634 | 94 | 2089 | 2116 | 4379.26 | 5118.20 | 582.69 |

- Storm 4 reaches 5,368,004,608 bytes at line 25609, only 704,512 bytes
  (0.67 MiB) below the 5,368,709,120-byte limit. Presentations then stop.
  Its crash-log heartbeat peaks at 5119.54 MiB (line 26205, both dates),
  memory level 2, with `device_lost=0`. Buffer heaps, committed resources,
  and a blit attachment heap return `0x8007000e`; `bad allocation` follows.
- Connections reports `bad allocation` at line 7574 and 4709.69 MiB at line
  7577. Its second run also fails, at 4703.80 MiB. These are periodic samples,
  not the peak of a failing transient allocation. The snapshots do not prove
  why an allocation fails with roughly 410 MiB still available.
- `D3D12_FEEDBACK memory_bytes` is `Context::MemoryUsage()`: accounted buffer
  heaps, staging and image resources together, **not** texture-cache bytes alone.
  In the cold Storm 4 run it grows from 94.81 to a peak of 582.88 MiB;
  the last feedback sample is 568.75 MiB. Its feedback-copy pool is only
  18.81 MiB at the last sample. Connections grows from 94.81 to 156.25 MiB
  and has no feedback-copy pool allocations.
- A cold shader cache starts with zero prewarmed records in both games.
  Storm 4's warm run builds 339 records and raises process commit from 2089
  to 2113 MiB during shader loading (24 MiB); it still reaches the limit.
  The last emitted JIT-code counters in the cold Storm 4 run total 169.75 MiB;
  Connections totals 82.96 MiB. These count emitted code, not all JIT allocations.
- Neither date contains `SPARSE` or `GUEST_MEMORY` samples. Their current
  instrumentation requires `NXBOX_VMMAP=1`. There are no separate live texture,
  buffer, shader-object or pipeline-object byte counters in these captures.
  Most of Connections' process usage is outside accounted D3D12 resources, but
  assigning that remainder to guest heap commit would be speculation.
- Later Storm 4 runs load at 934–1017 MiB instead of 2089 MiB, yet their first
  running samples still reach approximately 4.4 GiB. Lower load-stage commit
  alone does not establish a lower battle working set.

## Limit and existing fixes

`src/eden_uwp/game_session.cpp` reads the OS's `MemoryManager::AppMemoryUsageLimit()`.
Every request for 10, 8, 7 and 6 GiB is refused with `limit_now=5120 MiB`.
The 5120 MiB wall is the platform-granted UWP process budget, not a cap imposed
by `src/common`. `MemoryBudget::Limit = 512 MiB` in the D3D12 context is a
separate device-memory estimate used for cache policy, not the process limit.

Existing protections remain: demand-committed private guest DRAM (`fd6cc28a77`),
decommit instead of zero filling (`f0447fa4db`), sparse page-table storage, and
skipping the zero module tail (`54b394e116`). The new change does not alter guest
page contents, fault granularity, memory mappings, or the guest-visible heap size.

## Change

On UWP, refresh texture-cache GC watermarks each frame using the actual host
limit and usage. Subtract accounted D3D12 resources from process usage to estimate
other consumers, reserve 256 MiB for transient CPU/JIT/readback work, and cap the
normal 320/448 MiB watermarks to the remaining resource budget. The expected
watermark leaves another 64 MiB below that target. Arithmetic saturates instead
of wrapping when usage exceeds the limit. Returning host memory restores the
ordinary watermarks; unavailable telemetry falls back to 320/448 MiB.

For example, with limit=5120, process=4900 and accounted resources=156 MiB,
watermarks become 56/120 MiB, making the existing LRU react even though resources
remain below the old fixed 320/448 MiB thresholds. Existing dirty-image download,
decode protection, age thresholds and delayed destruction remain in charge.
This is a soft eviction policy, not a rejection of live allocations.

Both texture and buffer upload/readback rings drop completed, unleased pages
under this pressure. In-flight pages and CPU-held mapped spans remain alive.
Normal collection still keeps reusable pages. Empty underlying buffer heaps are
trimmed by the existing allocation-pressure path.

No new environment switch: the enabled behavior uses the existing eviction and
lease rules. No speculative guest-page or shader-discard optimization was added.
Desktop builds retain their previous thresholds and staging retention policy.

Changed implementation files:

- `src/video_core/CMakeLists.txt`: register the new header with the D3D12 target.
- `src/video_core/renderer_d3d12/d3d12_memory_budget.h`: portable host-budget math.
- `src/video_core/renderer_d3d12/d3d12_context.{h,cpp}`: OS adapter and staging trim.
- `src/video_core/renderer_d3d12/d3d12_texture_cache.{h,cpp}`: dynamic watermarks and staging collection.
- `src/video_core/renderer_d3d12/d3d12_buffer_cache.cpp`: staging collection under pressure.
- `src/video_core/texture_cache/texture_cache.h`: refresh optional runtime GC hook before collection.
- `tests/port/test_d3d12_host_memory_budget.py`: budget, OS fallback, per-frame GC and absent-hook regressions.
- `tests/port/test_d3d12_staging_pressure.py`: completed/in-flight/CPU-leased trim regressions.
- `docs/guest-memory-budget.md`: evidence and validation report.

## Validation

Commands run on the host, with clang++ and address/undefined-behavior sanitizers
for the new budget and staging drivers:

```text
python3 -B -m unittest discover -s tests/port -p 'test_d3d12_host_memory_budget.py'
Ran 3 tests — OK
python3 -B -m unittest discover -s tests/port -p 'test_d3d12_staging_pressure.py'
Ran 1 test — OK
python3 -B -m unittest discover -s tests/port -p 'test_t7_buffers.py'
Ran 4 tests — OK
python3 -B -m unittest discover -s tests/port -p 'test_load_module_bss.py'
Ran 2 tests — OK
python3 -B -m unittest discover -s tests/port -p 'test_host_memory.py'
Ran 4 tests — OK
python3 -B -m unittest discover -s tests/port -p 'test_d3d12_compile_policy.py'
Ran 2 tests — OK
```

The existing texture-transfer module exposes standalone test functions, so
unittest discovery found zero tests. They were then run explicitly:

```sh
python3 -B - <<'PY'
import sys, unittest
sys.path.insert(0, 'tests/port')
import test_d3d12_texture_transfer as module
suite = unittest.TestSuite(unittest.FunctionTestCase(getattr(module, name))
    for name in sorted(dir(module)) if name.startswith('test_'))
result = unittest.TextTestRunner().run(suite)
sys.exit(not result.wasSuccessful())
PY
```

Output: `Ran 3 tests` / `OK`. Total: 19 passing tests.

The new per-frame and staging regressions were also run with the old behavior
restored temporarily: both failed runtime assertions. Restoring the implementation
makes both pass. Changed C++ lines were formatted with clang-format and Python
files with ruff; `git diff --check` is clean.

Not verified: a full Windows/MSVC/UWP build, real D3D12 driver behavior, sustained
battle sessions on either Xbox, actual savings, visual correctness across titles,
or elimination of Connections' allocation failure. Earlier eviction can trade
memory for reload/readback work; live resources may still exceed the host budget.
The supplied logs cannot establish a complete allocation breakdown or prove that
this renderer-side reduction alone resolves either game.

Commit not created: `git add` cannot create the worktree `index.lock` because
the shared Git metadata is outside the sandbox writable roots. All changes
remain unstaged on the current branch; nothing was pushed.
