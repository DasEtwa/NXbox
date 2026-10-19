# Graphics PSO cost experiment

`tools/nxbox/pso_census.py` reads the eight supplied c24 cache folders without
modifying them. `pso-census.txt` contains the complete field census and the
PSOs-per-shader-set frequency distributions. Each pair is `(PSOs, shader sets)`.
Guest stage hashes identify shader sets; exact emitted HLSL, descriptor counts,
root mode and stream-output metadata constrain native equivalence. A shared guest
shader set alone is insufficient to merge pipelines.

| Game | Graphics records | Shader sets | A: request builds removed | A: native recipe merges | B: additional topology merges | Final distinct native recipes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| MK8D | 1246 | 631 | 248 | 211 | 0 | 1035 |
| BotW | 1413 | 351 | 802 | 782 | 0 | 611 |
| Odyssey | 1513 | 564 | 618 | 618 | 2 | 893 |
| Wonder | 1214 | 586 | 311 | 265 | 0 | 791 |
| Kirby | 779 | 251 | 406 | 355 | 0 | 410 |
| Bowser's Fury | 999 | 405 | 167 | 210 | 0 | 757 |
| DBSZ | 495 | 181 | 173 | 122 | 0 | 288 |
| LA | 26 | 21 | 0 | 0 | 0 | 26 |

These are offline equivalence counts, not measured console speedups. The archive
also contains duplicate native recipes with differing request specializations;
request reductions and native reductions describe different layers and must not
be added. Native sharing excludes stream output and advanced depth/stencil
pipelines; their wrappers continue on the existing path. Schema-6 recipes migrate
in memory: normalize graphics recipe/request state, retain the native/DXIL key,
and rewrite as schema 7 after successful prewarm. Duplicate legacy requests are
removed after admission finds an existing request. Compute sentinel state stays
unchanged. DXIL ignores the recipe schema and survives this migration.

A normalizes depth write/function when depth is disabled; stencil operations and
masks when stencil is disabled; unused back-face masks on the legacy stencil
path; blend equations when blending is disabled and unused target blend state;
disabled logic operations; patch vertex count outside patch topology; and the
instance divisor of per-vertex attributes. Attribute offsets, source slots,
conversions and enabled input formats remain binding metadata because the
rasterizer uses them. Existing `ApplyVertexReads` already removes unread shader
inputs. We retain bound RTV formats and masks for missing pixel exports rather
than infer an unwritten output from an unavailable or changed signature.

B removes exact IA topology from the **native** identity. Wrappers retain topology,
shader specialization and binding metadata. The command list still uses each
wrapper's exact topology. Native sharing requires the complete same remaining
recipe, so topology-dependent HLSL does not merge. Concurrent builds use one
shared future. Failed builds release the cache entry before notifying waiters,
so a later foreground request can retry a transient driver failure. This is an in-memory PSO cache, not a driver blob/library cache.
Blend factors, stencil references, depth-bound values, viewports and scissors are
already absent from the fixed key. Their additional B reduction is zero.

`NXBOX_D3D12_PSO_NORMALIZE=0` restores old descriptions, request keys, schema 6
and disables native sharing. `NXBOX_D3D12_PSO_FIRST_USE_ORDER=0` restores filename
ordering. Both flags are latched. First-use order includes foreground compiler
admission and prewarmed hits. A separate small mutex protects the session map;
header ranking and disk I/O never hold it. Workers append `rank key` lines in
`first-use-v<schema>.txt` during recipe writes and after prewarmed-only hits.
Session markers place the latest session first and retain earlier order as a
fallback. Persistence does not require Flush or a clean exit. Missing historical
order falls back to filename order. Normalized stalled requests use
`<title>_d3d12_stalled_pso_v3.txt`; matching legacy recipe hashes and unchanged
compute request hashes carry over from v2 during prewarm.

`NXBOX_D3D12_DXC_OPT=0|1|2|3` is latched, defaults to 3 (also on invalid input),
and selects `-O0` through `-O3`. Non-default levels salt both memory/disk DXIL
identity; default O3 identity is unchanged. Recipes contain HLSL and are compiled
with the selected level, including during prewarm. The native sharing map lives
only for the current title/process, where optimization level is constant.

DXC still compiles with validation enabled, checks compilation/validation status,
and returns the DXIL container; there is no `-Vd` or unsigned-DXIL path added.
`prepare_dxil.py` supplies `dxil.dll`, preferring the Windows SDK validator because
the release validator's desktop CRT cannot load in UWP. DXC invokes that validator
to validate/sign its result. All 5457 supplied DXIL containers have a nonzero
container digest (formats 1/2 use payload offset 76, format 3 uses 92); this header
inspection is not independent cryptographic/driver validation. `-Ges` enables
strict language diagnostics and `-HV 2021` selects language semantics; neither is
a signing switch. Both remain unchanged. Whether this entire emitted corpus
could safely use older language semantics or relaxed diagnostics is untested.

`D3D12_PSO_PREWARM status=complete` now reports the load-screen wall time,
worker target, disk hits/misses, thread-local memory hits, summed DXC/root/driver
milliseconds, and actual driver call count. Driver timing wraps only the native
API, excluding disk I/O and wrapper construction. Phase times sum concurrent
workers and can exceed wall time. `enumerated` and `interrupted` are explicitly
partial snapshots; a loading screen that waits for all workers logs `complete`.

## Console protocol (tester only)

No package/CI run exists for this uncommitted worktree. The lead must review,
commit and build/package it before a tester run.

1. Fully close the app between configurations; flags are read once. Set these
   lines in the package's `LocalState/nxbox_env.txt`:
   ```ini
   NXBOX_RENDERER=d3d12
   NXBOX_D3D12_FRAME_PROFILE=1
   NXBOX_D3D12_PSO_TRACE=1
   NXBOX_D3D12_PSO_NORMALIZE=1
   NXBOX_D3D12_DXC_OPT=3
   NXBOX_D3D12_PREWARM_THREADS=4
   NXBOX_D3D12_STATE_VERIFY=1
   ```
   Preserve unrelated settings. Keep native blobs/libraries disabled. Enable the
   existing `shader_preload` setting, async shaders and the same wait/skip policy
   as RC 780. `STATE_VERIFY` checks the state-tracker fast path.
2. Test the six combinations `PSO_NORMALIZE={0,1}` × `DXC_OPT={3,1,0}`. Back up
   the title's writable `ShaderDir/d3d12_cache/<title-id>` directory outside that
   directory; use a separate initially empty cache for each combination. Do not
   alter the supplied `*-orig` artifacts, saves, content or storage settings.
3. MK8D cold: play exactly the same route for five minutes. Collect diagnostics
   and frame profiles. Count completed graphics `PSO_TRACE` builds and sum final
   build wall times; sum `PSO_BUILD_COST driver_builds` and its phase times
   to distinguish shared wrappers from driver calls. Sum/count
   `PSO_WAIT kind=graphics` and `wall_ms`; record `draws_skipped_pending_pso`.
   Compare `pso_compiles`, `shader_translations`, `dxc_compiles`, `dxc_compile`,
   `pipeline_lookup` and `pso` with RC 780's 574 builds/44.2 s, 731 waits/23 s,
   337 skips as context, and primarily with the paired unnormalized run.
4. Close cleanly to flush the cache/order journal. Relaunch twice without
   changing the configuration or cache. Measure `D3D12_PSO_PREWARM status=complete
   ms`, `driver_builds`, disk hits/misses, `dxc_ms`, `root_signature_ms`, `driver_ms`
   and `threads`. A true warm repeat should have zero disk misses unless shaders
   were not persisted; memory hits are separate. Do not compare an O3-populated
   cache's first O1 launch against a warmed O3 launch.
5. Verify menus, first race, track transitions, HUD, shadows, transparency and
   instanced geometry against the paired baseline. No state-verifier mismatches,
   new DXC validation errors, device removals or regressions are acceptable.
   Repeat an Odyssey strip/list-heavy scene to exercise the two B merges.
6. To check first-use order, play early requests and terminate without a clean
   exit, then relaunch and skip the
   loading screen after a fixed short interval. Play the same first minutes;
   compare waits/skips to `PSO_FIRST_USE_ORDER=0` with an identical cache copy.
   The rank journal must prioritize early foreground requests despite reversed
   compile completion times (also covered by the host test).

Success: fewer cold request/driver builds and lower total build/wait/skip cost
with identical pixels; fewer warm driver calls and shorter repeat preload wall
time with unchanged thread count. Lower DXC levels are experimental: accept one
only if compile savings outweigh driver/runtime GPU cost without rendering or
frame-time regression. Offline archive counts do not predict a fixed reduction
for a different five-minute route. Hardware validation and Windows compilation
remain outstanding.
