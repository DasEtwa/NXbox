# Dragon Ball Sparking Zero: buffer aliases and JIT capacity

The supplied captures establish a dropped compute dispatch and substantial CPU/JIT cost.
They do **not** establish a locked 60 fps, or prove that one dispatch explains every black
surface. No post-change Xbox run or MSVC build was available in this worktree.

## Evidence read directly

Root: `/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18`.
Read `all-dbsz/diag.txt`, `ab-dbsz-prof/diag.txt`, and the `diag.txt` or
`eden_uwp_diag.txt` files in `dbsz-718`, `dbsz-eula`, `dbsz-lib`, `dbsz-live`,
`dbsz-sync`, `dbsz-fix`, `dbsz-rc`, and `dbsz-final105`. Inspected the two shots in
`ab-dbsz-prof/shots`. In particular, `shot-0190s.png` already has a blue sky and
textured canyon walls, with some solid-black rock geometry. The older symptom
description must not be treated as the state of every later capture.

Using the provided helpers (`python3 -I <helper> name=<diag.txt>`):

| Capture | Live windows | Median game fps | Peak memory |
| --- | ---: | ---: | ---: |
| all-dbsz | 30 | 18.8 | 4675 MiB |
| ab-dbsz-prof | 26 | 20.5 | 4604 MiB |

These whole-run figures include loading, logos, menus, and battle; they are not
controlled A/B measurements. `winstat.py prof=<root>/ab-dbsz-prof/diag.txt@160:180`
selects three late live windows: median game fps 12.1, submit CPU 3.14 ms/frame,
GPU-thread CPU 34.1%, and peak memory 4593 MiB.

### Dropped pass

`ab-dbsz-prof/diag.txt:4303` reports:

```text
D3D12_COMPUTE_STATE_CONFLICT have=0x8 want=0x1 dim=1 size=786432x1x1 mips=1 format=0 flags=0x4 uses=6
```

The same conflict repeats through line 4643. `all-dbsz` reports the same state pair
on a 262144-byte buffer. This is a buffer UAV versus constant-buffer conflict,
not a failed shader compilation or an EQUAL depth comparison. Earlier captures
also contain graphics SRV/UAV conflicts (`0xc0` versus `0x8`), which the existing
buffer-state merging already handles.

`DispatchCompute` already snapshots constants aliasing raw storage buffers, but
`IsStorageBuffer` only examined `stage.storage`. Image-buffer UAVs live in
`stage.texel`, and their identity was lost in `BindTextureBuffer`. Consequently a
constant buffer aliasing an image buffer escaped the snapshot and caused
`ComputeDispatch::Bind` to reject the dispatch. Read-only image accesses also use
UAV descriptors and need the same protection.

The fix preserves image-buffer identity through the generic buffer-cache binding
hook, then includes those bindings in the existing constant-snapshot decision.
Both graphics and compute paths use it. Ordinary texel SRVs do not gain copies.
This is enabled by default: it extends an existing correctness mechanism without
relaxing the state-conflict checks. The logs lack shader/program identifiers for
the rejected dispatch, so its exact visual contribution remains to be confirmed
on the console; this is a concrete missing producer, not a proven full visual fix.

Global-memory compute stays under the existing policy. `c94918bbfc` deliberately
restored the default skip after `aa91114f9c` caused black MK8D races. Re-enabling
all such programs or relaxing EQUAL globally is not justified by these logs.

### Largest measured consumers

| Evidence in ab-dbsz-prof/diag.txt | Measurement | Interpretation |
| --- | --- | --- |
| GAME_STALL, line 4238 | JIT 3685 ms / 110203 calls; 224410 blocks cleared | Capacity evacuation and subsequent compilation during transition |
| GAME_STALL, line 4299 | JIT 5447 ms / 155165 calls; 225766 blocks cleared | Another capacity evacuation; emission instructions alone total 3209 ms |
| GAME_STALL, lines 4411 / 4531 / 4645 | JIT 3687 / 1765 / 1867 ms per window | Continued compilation in battle, not solely GPU rendering |
| FRAME_PROFILE, lines 4398 / 4514 / 4634 | Hardware GPU 7.32 / 7.67 / 7.44 ms per presented frame | Physical GPU time is much smaller than observed frame intervals |
| Same three windows | Submit CPU 3.71 / 3.05 / 3.09 ms/frame; 42.67 / 43.59 / 44.43 guest-fence submits/frame | Secondary CPU submission cost; existing batching experiment remains off |
| Guest waits, last window | GPU queue idle 42.37 ms/frame; nvdrv event waits 30.70 ms/frame | CPU supply and scheduling matter; these waits overlap other threads |
| THREAD_PROFILE, late windows | CPUCore_0 up to 82.19%, CPUCore_2 up to 78.12% | Busy emulation cores while the GPU queue often has no work |

JIT totals sum work across emulation cores; nested phase timings must not be
added to their parents. Likewise `sync_wait_cpu_ms` includes GPU queue idle time
and must not be described as hardware GPU time. No sampled instruction stacks
are present in this capture's diag/crash/eden files; thread percentages alone
cannot attribute the remaining guest execution to a specific function.

## Opt-in capacity experiment

`NXBOX_JIT_LARGE_CACHE=1`, checked through `Core::NxboxJit::BisectEnabled`, gives
UWP A64 JITs 192 MiB instead of 128 MiB. It is latched at first use and defaults
off; only the exact value `1` enables it. Desktop, A32, and the 8 MiB null JIT
keep their existing limits. `NXBOX FAULT CONFIG` now prints `code_cache_bytes`.

This targets the measured capacity evacuations by keeping a larger working set
resident. It does not reduce first-time compilation or guarantee the game fits
at 192 MiB. Four cores add at most 256 MiB of cache capacity. Adding that to the
4675 MiB observed peak gives 4931 MiB, only 189 MiB below the nominal 5 GiB
process budget, before any additional variation. Therefore it must remain an
experiment, not a new default. A console A/B must compare `jitflush`,
`jit_clear_blocks`, JIT totals, frame windows, and peak memory along the same
route. There is no measured post-change fps gain yet.

## Changes and validation

- `d3d12_descriptor.h`, `d3d12_buffer_cache.h/.cpp`, `d3d12_rasterizer.cpp`:
  preserve image-buffer identity and snapshot aliased constants.
- `nxbox_jit_policy.h`, `arm_dynarmic_64.cpp`: bounded opt-in A64 capacity and
  effective-size logging.
- `tests/port/test_dbsz_buffer_alias.py`: execute the production binding method,
  buffer structure, and classifier with clang++ and ASan/UBSan; cover image UAVs,
  ordinary SRVs, cross-stage storage aliases, unrelated resources, and clearing.
- `tests/port/test_dbsz_jit_capacity.py`: compile the production policy, exercise
  unset/invalid/off/on environment values in separate processes, check latching,
  four-core budget, UWP integration, and null-JIT override.

Both new test files failed before implementation (missing hook/policy and old
binding selection); both passed afterward. Commands use
`python3 -B -m unittest discover -s tests/port -p '<pattern>'`:

| Pattern | Output |
| --- | --- |
| `test_dbsz_*.py` | Ran 4 tests; OK |
| `test_d3d12_uniform_storage_alias.py` | Ran 2 tests; OK |
| `test_d3d12_compute.py` | Ran 1 test; OK |
| `test_d3d12_root_cbv.py` | Ran 5 tests; OK |
| `test_d3d12_copy_write_hazards.py` | Ran 7 tests; OK |
| `test_d3d12_feedback.py` | Ran 3 tests; OK |
| `test_d3d12_buffer_fastpath.py` | Ran 2 tests; OK |
| `test_t10_jit.py` | Ran 7 tests; OK |
| `test_d3d12_compile_policy.py` | Ran 2 tests; OK |

`test_d3d12_upload_cpu.py` discovery reports zero tests, so it is not counted.
Formatted changed C++ ranges with the repository clang-format configuration and
new Python files with Ruff. `git diff --check` passed. Not verified: MSVC/Xbox
compilation, repaired-pass screenshots, post-change memory/fps, Series S GPU
scaling, and the locked-60-fps target. The Series X hardware timings cannot be
converted into a verified Series S frame budget.
