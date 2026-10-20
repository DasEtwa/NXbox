# Naruto Storm D3D12 rendering investigation

Scope: Storm Connections (`01005A20190A6000`) and Storm 4 Road to Boruto
(`01006CF00CF60000`), reported on Xbox Series S. The separate memory exhaustion
and freeze are outside this change.

## Evidence and fixes

Read all four files directly from `artifacts/elzar-2026-10-18/` and
`artifacts/elzar-2026-10-19/` under `/Users/miguel/.local/share/nxbox/`:
`eden_uwp_diag.txt` and `nxbox_crash.txt` in each directory. The later files
repeat earlier sessions, so repeated messages are not independent incidents.

The October 19 diagnostic log identifies Connections launches at lines 7354,
7441, 7593, 7774, 10956 and 14867, and Storm 4 launches at lines 19858, 25629,
38448 and 40956. It records `unsupported attachment or mismatched sample counts`
at line 25592 and `bad allocation` failures at lines 25610, 29120 and 40940.
Those messages do not establish the cause of the upside-down menu or missing
HUD. No per-draw guest viewport/origin values were captured in these logs.

Two differences from the Vulkan implementation are reproducible on the host:

- The guest `YDirection` special register must return -1 for lower-left window
  origin and +1 for upper-left origin (`EmitYDirection` in the SPIR-V backend).
  D3D12 previously supplied the signed-viewport emulation flip instead. Its value
  could change with viewport height or swizzle even while window origin stayed
  fixed. Root constants now use window origin; the separate final vertex-stage
  position negation continues to emulate the signed viewport.
- With viewport scale/offset disabled, Vulkan uses the surface clip as the scissor.
  D3D12 additionally intersected it with enabled scissor registers, allowing stale
  bounds from an earlier pass to hide portions of an untransformed pass. That
  intersection now applies only when viewport scale/offset is enabled.

The host regression compiles the actual viewport/scissor functions, root-constant
method, input structure and rasterizer call expression with clang++ and
ASan/UBSan. It checks 16 combinations of origin, viewport transform enable,
height sign and Y swizzle, including transformed scissors as a control. Before
the fixes it reports 12 incorrect YDirection values and 8 clipped untransformed
scissors; afterward it passes. This proves the backend differences, not that
these games execute either affected path at the reported moment.

The D3D12 blit shader keeps signed source/destination extents in its address
calculation and uses normalized bounds only for the destination viewport/scissor.
Presentation applies the layer's transform flags to UVs. Inspection did not
justify a global presentation/blit flip, speculative barrier, or clear change.

## Console captures still needed

Capture the menu and a battle separately, preferably before memory pressure.
For a selected one-based frame N, set these before starting the game:

```text
NXBOX_PASS_DUMP_FRAME=N
NXBOX_PASS_DUMP_DIR=<writable capture directory on the console>
NXBOX_D3D12_DRAW_TRACE_FRAME=N
NXBOX_D3D12_DUMP_HLSL=1
NXBOX_D3D12_PSO_SKIP_TRACE=1
NXBOX_D3D12_COPY_TRACE=1
```

Use the same N for pass dumps and draw tracing. The UWP window supplies a local
pass-dump directory when `NXBOX_PASS_DUMP_FRAME` is set without an explicit
`NXBOX_PASS_DUMP_DIR`. Trace output is capped at 3000 draws for that frame.
HLSL dumps are written under the emulator log directory when shaders translate;
a cached shader might require a cold shader-cache run to produce its source.

Remaining hypotheses and evidence to collect:

- **Menu inversion:** a shader consuming guest YDirection may explain the image.
  Match `nx_y_direction` and VS hash in `D3D12_DRAW_TRACE` to dumped HLSL. Compare
  the first inverted pass output with its sampled inputs and presentation record
  to distinguish a rasterization error from a blit or compositor transform.
- **Flicker:** a sampled attachment may contain an earlier pass/frame, or a draw
  may be skipped while its asynchronous pipeline compiles. Compare pass dumps
  from adjacent frames (separate runs), feedback counters, and
  `NXBOX_D3D12_PSO_SKIP_TRACE=1`; add `NXBOX_D3D12_PSO_TRACE=1` for compilation
  timing. Existing logs show feedback copies but do not prove a missing barrier.
- **Missing battle HUD:** check whether affected draws use the untransformed
  scissor path, have empty scissors, fail attachment validation, or fail the
  depth/stencil test. The draw trace includes viewport, scissor, attachment
  formats and native PSO depth/stencil state. `NXBOX_D3D12_DEPTH_TRACE=1` adds
  bounded depth draw/clear diagnostics (only the first four of each).

For a short synchronization investigation, `NXBOX_D3D12_SUBMIT_TRACE=1` logs
submissions/API state and waits for completion. Run this separately: serialization
changes timing and can hide flicker. A change under serialization is evidence to
investigate, not proof of a barrier fix.

No Xbox/MSVC build or console capture was available on this macOS host. Visual
resolution of menu inversion, flicker and the battle HUD remains unverified.

## Host validation

Each pattern below was run with
`python3 -B -m unittest discover -s tests/port -p "<pattern>"`:

| Pattern | Output |
| --- | --- |
| `test_d3d12_window_origin.py` | `Ran 1 test` / `OK` (original code: `FAILED (errors=1)`) |
| `test_d3d12_state_coverage.py` | `Ran 5 tests` / `OK` |
| `test_d3d12_draw_contract.py` | `Ran 1 test` / `OK` |
| `test_d3d12_redundant_state.py` | `Ran 8 tests` / `OK` |
| `test_d3d12_draw_trace.py` | `Ran 6 tests` / `OK` |
| `test_d3d12_state_tracker.py` | `Ran 4 tests` / `OK` |

`test_d3d12_texture_transfer.py` defines free test functions, so unittest discover
reported `Ran 0 tests` / `NO TESTS RAN`. The following adapter then executed all
three functions successfully (`Ran 3 tests` / `OK`):

```sh
python3 -B - <<'PY'
import importlib.util
from pathlib import Path
import unittest
path = Path('tests/port/test_d3d12_texture_transfer.py')
spec = importlib.util.spec_from_file_location('texture_transfer', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
suite = unittest.TestSuite(unittest.FunctionTestCase(getattr(module, name))
                          for name in sorted(dir(module)) if name.startswith('test_'))
result = unittest.TextTestRunner().run(suite)
raise SystemExit(not result.wasSuccessful())
PY
```

Ruff formatted the new Python test. Clang-format checks of changed C++ lines
and `git diff --check` passed without output. Existing files were not reformatted.
