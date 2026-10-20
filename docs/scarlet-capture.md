# Frame 1200 UAV/copy investigation — 2026-10-20

This section covers `compat-2026-10-18/v-scarlet-cap` and supersedes absence
claims based only on the older captures. A cache-coherence defect is fixed below;
**a Scarlet rendering cure is not established**.

## Evidence and its limits

- The manifest has five attachment snapshots, not five proven draw passes.
  Clears also update render targets and create snapshots. Pass 5 is at
  `22951559168` (`0x558050000`), 1920×1080 `B10G11R11_FLOAT`.
  All 2,073,600 packed words are `0x591642c8`: RGB is exactly 0.0703125.
  The snapshot therefore contains only the reported clear value.
- Presentation selects `23033741312` (`0x55ceb0000`), 1920×1080
  `A8B8G8R8_UNORM`, with no attachment-snapshot producer (`producer_seq=0`).
  The old dumper observes neither Fermi2D destinations nor shader storage images.
  This does not distinguish a copy from an image store, or prove either lost data.
- Eden contains events 0–66, followed by `frame_end events=67 truncated=false`.
  There is one `guest_draw` and one host draw submission: a 3-vertex, 6-instance
  draw to `0x54af60000`, 64×64 `R16_FLOAT`, PS `fa4b2ee1b67cfe3e`,
  VS `0f38f8c713b9ff63`. There is no guest compute, indirect draw, compute
  submission or draw-skip record in this selected trace. In particular there is
  **no traced compute writer to either requested address**.
- Live profiles report about seven accelerated Fermi2D blits/frame, zero software
  Fermi blits, and an empty guest compute pipeline cache. The run-wide rate of
  fourteen guest draws/frame disagrees with the single selected-frame draw; it
  does not identify the unobserved draws or prove every frame contains them.
  The old submission trace did not record successful Fermi blits or DrawTexture.
  Their actual source/destination addresses remain unknown for this capture.
- The supplied `v-scarlet-cap` tree contains no `.hlsl` or `.hash` files, despite
  the HLSL-dump env being enabled. There is no supplied shader source with which
  to identify image stores, manually packed pixels, or the final conversion.

Consequently the existing files cannot identify the writer of the presented
image or establish an R11G11B10 typed-UAV, transition, temporary-copy, or
recreation failure. Do not change packed-format storage representation on this
evidence. A prior-frame writer is also possible; a single selected frame does
not prove the scene is rewritten every frame.

## Concrete cache defect and correction

D3D12 prepared descriptor images through `VisitImageView`, which calls
`PrepareImageView(..., false, false)`. Neither the graphics nor compute image-UAV
binding path subsequently called `MarkModification`. Vulkan's
`PushImageDescriptors` does so for every `desc.is_written` image.

D3D12 now collects writable image views and publishes their modifications after
successful draw/dispatch recording, before diagnostic readback. This sets
`GpuModified` and advances `modification_tick`. Without that publication,
`SynchronizeAliases` can regard a shader output as older than its alias, and
`IsDownloadable` can ignore its GPU-only changes. Image writes now retain the
texture-cache lock through publication. Read-only storage views and failed/zero
work do not publish writes. Existing D3D12 compute UAV barriers remain in place;
this change does not add an unproven format alias or barrier workaround.

This fixes a real missing-write coherence contract, independently of capture
settings. It has **not** been linked to a shader in the supplied Scarlet frame.

## Next capture and expected proof

Use the existing exact-env/collection instructions below, with the newly built
package. In addition to attachment snapshots, `manifest.jsonl` now records
`writer=compute_uav`, `graphics_uav`, `fermi_blit`, or `draw_texture` after the
corresponding work. Each record gives the cache-image address, image/view format,
mip, layer, dimensions, raw filename and byte count; compute includes the guest
program offset, and blits include `source_gpu_addr`. Shader images are marked
modified before any capture wait. Each 1D/2D view mip/layer gets its own
`pass_NNNN_c0_mM_lL.raw`; volume/MSAA writes are explicitly skipped rather than
represented as empty successful raws. Successful captures participate in the
presentation producer lookup. Submission logs now name Fermi source/destination
addresses and DrawTexture source/destination addresses, including Fermi fallback
status. Alias synchronization now reports both addresses and the source modification
tick to the diagnostic sink; image retirement reports its address, tick and dirty
state. Presentation producer matching checks mip/layer ranges, so a later write
to an unrelated mip cannot claim the displayed image. Previous pending attachment
snapshots close before compute/blit writes.

Follow the last record at each address backward through blit sources and shader
program/hash mappings. Require the entire fresh `log/gfx_*.hlsl` and `.hash` set;
the env alone is not shader evidence. If no compute is requested again, expect no
`compute_uav` records: follow `fermi_blit`/`draw_texture` instead. A nonzero
presentation producer and its actual raw pixels should identify the final writer.
Compare the scene's post-writer pixels with its clear snapshot. Nonconstant scene
pixels would support progress; a still-clear scene with only copy producers
moves the investigation upstream, rather than proving a typed-UAV problem.
If neither requested image is written, repeat a neighboring frame with a clean
capture directory, including the same shaders.

Host regressions execute the production publication and view-readback methods:
capture-disabled dirty flags/ticks, null/buffer exclusion, multiple mip/layer
bytes and metadata, unsupported volumes, and failed output paths. Xbox/UWP
compilation, D3D12 validation and console rendering remain unverified.

---

# Scarlet capture audit — 2026-10-20

This section supersedes the older run instructions below. No rendering fix or
performance improvement is established by this audit.

## What actually fired

Both requested frames were reached. `z-scarlet-capture` contains one
`D3D12_DRAW_TRACE frame=3000` and one `D3D12_PASS_PRESENT frame=3000`;
`z-scarlet-cap2` contains the same pair at frame 1200. Counting their copies in
`diag.txt`, `eden.txt` and `crash.txt` as separate draws would triple-count them.
The cache clock starts at zero; `CaptureFrame()` is `frame_tick + 1`, and
`TextureCache::TickFrame()` closes the selected pass before incrementing it.
D3D12 calls that method from `RendererD3D12::Composite`. Pass dumping is hooked
from both dirty and unchanged render-target updates, and from presentation.
There is no evidence here that either capture missed its frame counter.

**The archived directory does not match the configured destination.** Both
archives contain an empty `passdump/`, but the env points to
`LocalState/scarlet-pass-3000` / `LocalState/scarlet-pass-1200`. Neither archive
contains those directories. There is no `Pass dump ... failed` message in their
Eden logs. This establishes a collection mismatch, not proof that the console
wrote zero files. Q:/ directory creation has not been tested on the host and
must not be declared the cause without a filesystem error from the console.

The instrumentation now probes the actual destination at startup, writing
`capture.txt`, and emits `PASS_DUMP ready frame=... dir=...` or
`PASS_DUMP directory failed ... error=...` to diag. Completed raw writes emit
`PASS_DUMP wrote frame=... seq=... bytes=... file=...` **after close**.
Presentation closes the manifest before emitting `PASS_DUMP manifest ...`.
Raw/manifest failures also reach the D3D12 diagnostic sink independently of the
Eden log filter. The normal capture path, shaders and rendering decisions are
unchanged. These are diagnostic changes, already gated by the existing capture
env; no speculative renderer bisect was enabled.

## What the gray-frame evidence permits

Each selected frame has a complete submission trace: events 0–78,
`frame_end events=79 truncated=false`. It contains one `guest_draw`:
nonindexed, 3 vertices, 6 instances, `execute=true`, condition mode 0; the host
submits it with `active=0` predication. Its target is `0x54af60000`, 64×64,
`R16_FLOAT` (DXGI 54), six layers. The presented cache image is a different
1920×1080 `A8B8G8R8_UNORM` image at GPU address 23033741312, with
`producer_seq=0`, `producer_slot=-1`. That means no captured producer was found;
it does not prove that the compositor chose the wrong image.

Neither selected trace contains guest indirect draws, guest compute, condition
suppression, PSO drops, or query report/wait events. The `query_flush_begin/end`
pairs complete. No `D3D12_QUERY_RESOLVED` or `D3D12_PREDICATE` completion appears
in diag. Prewarm builds 29/29 PSOs, zero failures; live profile windows report
zero pending-PSO draw/dispatch drops and zero guest compute cache entries.
Dispatch totals alone include backend query helpers and do not demonstrate
missing guest compute.

There is a material discrepancy: whole-run profiles count **14.0036** and
**14.0030** guest draws/frame (4,139 and 5,040 frames), while each isolated trace
has one draw. Do not extrapolate those isolated frames into a claim that the
game never submits its other draws, or name an incorrect fullscreen pass without
collecting its actual outputs. The early guest read fault in cap2 is at
8.182 s, PC `0x8177e0f8`, address `0x11c422867f`, well before frame 1200 at
55.409 s. It remains a separate CPU-side lead. The exact-conditional plain run
also remains gray; the older AND/OR predicate defect is not a demonstrated cure.
The supplied artifacts do not establish a first wrong render pass, an incorrect
present selection, a dropped compute pass, or an unsupported-format conversion.

## Next console run — exact env and collection contract

Build/package the current branch first. Terminate the app and archive/remove
old `LocalState/passdump` and logs. Replace `nxbox_env.txt` with exactly:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_THREAD_PROFILE=1
NXBOX_D3D12_DRAW_TRACE_FRAME=1200
NXBOX_PASS_DUMP_FRAME=1200
NXBOX_D3D12_DUMP_HLSL=1
NXBOX_SUBMISSION_TRACE=1
```

**Omit `NXBOX_PASS_DUMP_DIR`.** Startup resolves the real application LocalFolder
and selects its `passdump` child, matching the archived collection directory.
Use `log_filter.txt` containing `*:Info` and `flush` on separate lines. Use the
same game/update/save/input route as cap2; run 90 seconds after launch (cap2
reached the selected frame at 55.4 s). Collect fresh diag/Eden/crash logs, a
screenshot, the **entire directory printed by `PASS_DUMP ready`**, including
`capture.txt`, `manifest.jsonl`, `sampled.jsonl`, all raw files, and all
`log/gfx_*.hlsl` / `log/gfx_*.hash`. A renamed empty local directory is not a
successful capture. Check that every `PASS_DUMP wrote ... file=...` exists in
the archive with the logged byte count. If startup or a write fails, retain its
full error; do not compensate by guessing another Q: path.

Require the frame-1200 presentation mapping and complete submission `frame_end`.
If the selected frame again contains only one draw, repeat with both frame
settings at **1201**, a clean output directory and identical inputs, to sample
its neighbor. Correlate the actual presented producer and sampled inputs before
changing renderer state. Do not add exact-conditional/CPU-predicate/no-compute
bisects to this baseline. Terminate the app and restore env/log filter afterward.

## Measured costs and validation limits

`winstat.py` over cap2's 34 live windows reports median game fps 30.0,
GPU-thread CPU mean 4.6%, stall GPU busy share 14%, peak 2082 MiB. Across its
5,040 profiled frames, largest inclusive phase times are sync wait **29.071**,
Fermi2D **1.404**, Maxwell methods **0.648**, and DMA pusher **0.419 ms/frame**.
Capture 3000 has sync wait **29.009** and Fermi2D **1.437 ms/frame**.
These are inclusive wall scopes, not additive CPU usage. Removing sync waits
without proving their dependencies unnecessary is not an optimization justified
by these logs. No measured reduction or locked-60 claim is made.

The other supplied captures reproduce the historical table below: BOTW 12.9 fps,
sync wait 13.234 / draw recording 6.972 ms; Odyssey 24.7 fps, Maxwell methods
9.498 / sync wait 4.923 ms; MK8D 31.8 fps, sync wait 5.743 / Maxwell methods
3.828 ms; DBSZ 18.8 fps, pipeline lookup 6.490 / sync wait 6.278 ms.
`dbsz-final105` supplies `eden_uwp_diag.txt`, not `diag.txt`.

Host clang++ tests execute the actual raw-write lambda, presentation writer,
diagnostic forwarding and filesystem probe, covering successful bytes, closed
outputs, mismatched frame selection, missing directories and blocked output
files. The raw and manifest reporting regressions fail against the original
production bodies. Xbox/UWP MSVC compilation, actual Q: filesystem access,
console readback, the next capture and Series S performance remain unverified.

---

# Historical investigation (through 2026-10-19)

# Scarlet: trace the presented image and missing scene draws

This is a diagnostic capture, not a rendering fix. The prior capture showed empty
scene depth/G-buffer inputs, only 14 procedural/fullscreen-looking draws, and a
constant HDR chain. Pass 15 was the first useful suspect, not a proven first wrong
pass. Correct sRGB sampling explains the later 148 -> 76 conversion. Constant
pixels alone do not establish a bug in a shader or pass.

## One console run

The lead must build/package this change first; no new package was built locally.
Use Scarlet `0100A3D008C5C000`, the same update/save/camera as the previous capture,
and the native D3D12 backend. Fully terminate the app before replacing files.
Remove old capture output and archive old logs/HLSL so files from another process
cannot satisfy the hash check. Keep the disk PSO cache: this run tests prewarming.

Replace `LocalState/nxbox_env.txt` with these literal lines:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_D3D12_DRAW_TRACE_FRAME=120
NXBOX_PASS_DUMP_FRAME=120
NXBOX_PASS_DUMP_DIR=Q:/Users/UserMgr0/AppData/Local/Packages/NSPX.NXbox_xb0tjvbn8pcrt/LocalState/scarlet-pass-120-mapped
NXBOX_D3D12_DUMP_HLSL=1
NXBOX_SUBMISSION_TRACE=1
```

Replace `LocalState/log_filter.txt` with these two lines, so a hang retains the
last query/condition event:

```text
*:Info
flush
```

Launch once, reproduce the gray scene, and wait at least 60 seconds / until frame
120 completes. Collect the screenshot, fresh `eden_uwp_diag.txt`, `nxbox_crash.txt`,
Eden log, the entire `scarlet-pass-120-mapped` directory, and all `log/gfx_*.hlsl`
and `log/gfx_*.hash`. Stop the app after collection. Remove the diagnostic env
lines and restore the previous log filter after testing.

Do not set `NXBOX_D3D12_NO_PREWARM`, `NXBOX_D3D12_CPU_PREDICATE`, or other renderer
bisect overrides in this run. Disabling prewarm would avoid testing the fixed
path; forcing CPU predication changes the behavior being measured.

## Follow the actual presented image backward

1. Find `D3D12_PASS_PRESENT` in diag. The identical JSON object has
   `"event":"present"` in `manifest.jsonl`. Each presented layer gets a record.
   `cpu_addr` is the display's address plus offset; `gpu_addr`, `format`, `width`
   and `height` identify the selected cache image. `view_*` describes the view.
   `producer_seq` / `producer_slot` name its last captured pass output, including
   a final pass still pending when presentation starts. Addresses in JSON are
   decimal; draw traces print hexadecimal.
2. Open `pass_<producer_seq padded to four digits>_c<producer_slot>.raw`. Follow
   its `sampled` addresses to earlier output entries and `sampled.jsonl` mip/face
   captures. Use the latest preceding writer of an address, not the first one.
   `source=guest_memory`, `producer_seq=0`, or `producer_slot=-1` explicitly means
   there is no captured pass producer; do not substitute the old guessed pass 72.
3. For every `ps_hash` / `vs_hash` in `D3D12_DRAW_TRACE frame=120`, require
   `gfx_<16-digit guest hash>_*.hlsl` plus its matching `.hash`. The sidecar records
   `guest_hash`, `source_hash` (FNV-1a of the dumped HLSL), and numeric stage.
   Both prewarmed and fresh graphics/compute shaders are dumped. Additional files
   ending in a source hash preserve different PSO specializations of one guest
   stage; inspect all matching variants with the trace's attachment/PSO state.
4. Name a first wrong pass only when its inputs are correct and its actual shader
   and draw state require a different output, or a synchronized good reference
   proves the earliest divergence. `tools/nxbox/pass_diff.py good bad --out diff`
   compares raw outputs and ignores presentation event records. An unchanged
   intentional clear, a uniform sky with empty depth, and the final sRGB
   conversion are not sufficient evidence. Pass numbering may change this run.

## Test the "no scene geometry" hypothesis

`NXBOX_SUBMISSION` lines are in the Eden log, restricted to the same one-based
frame as `NXBOX_D3D12_DRAW_TRACE_FRAME`. `frame_end` must exist with
`truncated=false`; `TRUNCATED limit=20000` makes absence claims inconclusive.
The instrumentation observes guest methods; it does not bypass decisions.

- `guest_draw` is emitted before Maxwell's CPU `ShouldExecute()` gate and
  includes indexed/count/instances, condition address, mode and override.
  `execute=false` proves the guest did issue a draw that CPU conditional rendering
  suppressed. `condition_begin/end` and `condition_values` expose the comparison.
- `guest_indirect` observes the same gate for indirect draws. `host_indirect`
  shows the argument/count addresses, downloaded guest count and expanded count.
  Zero expansion after a compute dispatch is evidence for the argument producer
  path; it does not by itself prove that compute produced the wrong value.
- `host_draw_begin/end`, `host_draw_drop reason=pending_or_failed_pso`, and
  `host_skip reason=...` distinguish host-side drops from guest absence.
  `host_draw_submit draw=N pass=P` correlates with `D3D12_DRAW_TRACE`. `draw=0`
  means no matching draw-trace record (its separate limit is 3000 draws).
- `host_predicate ... active=0` means no native predicate is bound. Otherwise
  `D3D12_PREDICATE frame=120 pass=P draw=N value=... op=... executes=...` appears
  in diag after GPU completion. It allocates readback storage before
  binding draw state, then copies the actual predicate bytes immediately before
  that draw, temporarily unpredicated, and restores predication. The copy adds no
  explicit CPU fence wait or forced predicate value; allocation can submit/wait
  under memory pressure. `executes=false` proves
  native predication suppressed that submitted draw. Missing completion lines
  are inconclusive if the queue never completes.
- `guest_compute`, `compute_shader guest_hash=... request=...`, and
  `host_compute_begin/submit/end` identify requested compute, its shader dump,
  grid and host drops. A submitted dispatch alone does not prove correct GPU
  execution or correct storage writes. Examine its HLSL and the later indirect
  counts/depth/G-buffer inputs; zero grids or `submitted=false` are direct leads.
- `query_report_begin/end` records report address/type and pending snapshot state;
  `D3D12_QUERY_RESOLVED` in diag retains the originating frame/address/value even
  when completion arrives in a later frame. Compare it with the condition address
  and values. `query_*_wait_begin/end`, `query_bank_finish_begin/end`, and
  `query_flush_begin/end` bracket waits/delivery. An unmatched begin identifies
  the blocked path; an absent resolution alone does not prove a stuck query
  because invalidated/freed reports may legitimately never publish a result.

If the complete frame still contains only the 14 fullscreen draws, no extra
`guest_draw`/`guest_indirect`, and no predicated-out/CPU-gated scene draws, the
scene draws never reached the observed Maxwell draw methods in that frame.
That supports investigating guest command generation earlier (CPU/query/compute),
not declaring the renderer's fullscreen pass wrong. Geometry draws that appear
in any of those traces refute the claim that the guest never submitted them.
Nonempty scene depth/G-buffer outputs also refute the narrower empty-scene
hypothesis. A quiet frame cannot establish what the guest submitted in earlier
frames, or why it did not submit geometry.

If frame 120 is never reached, the absence test has not run. The exact follow-up
is to change both frame lines to the last progressing frame shown by the profile,
choose a fresh dump directory, terminate/relaunch, and compare paired wait/condition
markers there. If predication is implicated, a separate controlled run adding
`NXBOX_D3D12_CPU_PREDICATE=1` and a new directory can distinguish GPU/CPU predicate
coherence; it is not needed to observe predicate suppression in this run.

## Local audit, 2026-10-19: evidence and a conditional-rendering defect

The supplied artifact root is
`/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18`.
It contains `scarlet*` and `all-scarlet`; there are no `x-scarlet*` directories.
None of these captures contains the submission/predicate trace requested above.
`scarlet-passdump` has 14 traced draws at frame 120, 19 distinct VS/PS hashes,
**zero matching dumped HLSL files**, and no `.hash` sidecars. Its old manifest
also predates the presentation mapping. The next experiment above has not been
performed in these artifacts. A first incorrect fullscreen pass, a dropped guest
compute dispatch, and predicated-out geometry cannot be identified from them.

Across the 15 live profile windows of `scarlet-passdump`:

- 2,178 frames, 30,507 draws (14.0069/frame), 119 helper/compute dispatches;
- no recorded PSO-pending draw/dispatch drops or failed graphics entries;
- compute setup time is zero and the guest compute pipeline cache remains empty.
  The dispatch count is consistent with backend query reduction, not evidence of
  guest compute execution;
- median game fps is 29.9, GPU-thread CPU is 6.6%, and GPU-stall busy share is 20%.
  The GPU timestamp busy measurement is 5.950 ms/frame. These are distinct metrics;
- `sync_wait_cpu_ms` is 27.734 ms/frame, Fermi2D 1.792, submit 0.579,
  texture-cache update 0.511, barriers 0.252, and native fence wait 0.195.
  Phase scopes are inclusive wall times, not additive CPU costs;
- about 7.018 forced guest-fence submissions/frame. `query_buffer_copies` is a
  cumulative diagnostic, not a per-window count, so it must not be summed.

For context, whole-run live-window measurements from the other supplied captures
are below. These are different games/scenes, not an A/B comparison of this patch.
`dbsz-final105` uses `eden_uwp_diag.txt`, not `diag.txt`.

| Capture | Median game fps | Largest measured phase times, ms/frame |
| --- | ---: | --- |
| `x-botw-rec` | 12.9 | sync wait 13.234; draw recording 6.972; Maxwell methods 6.295 |
| `x-ody-rec` | 24.7 | Maxwell methods 9.498; sync wait 4.923; draw recording 2.522 |
| `x-mk8d-rec` | 31.8 | sync wait 5.743; Maxwell methods 3.828; draw recording 2.236 |
| `dbsz-final105` | 18.8 | pipeline lookup 6.490; sync wait 6.278; draw recording 2.818 |

The Scarlet Eden log also records a guest CPU read fault at 8.412 s:
PC `0x812160f8`, address `0x11c422767f`, before the first captured frame.
This is a separate lead, not proof of the gray screen's cause or of a D3D12 fault.
No runtime or console experiment was performed during this local audit.

### Exact single-condition experiment

Code inspection found a definite D3D12 predicate mismatch. Maxwell's
`ProcessQueryCondition` and Vulkan's `resolve_conditional_render.comp` require
both `initial_sequence != 0` **and** `initial_mode != 0` for `Conditional`.
D3D12 used a raw nonzero 64-bit predicate, implementing **or** instead of **and**;
its forced CPU predicate path used the same incorrect whole-value comparison.
For example, `{sequence=1, mode=0}` incorrectly rendered.

`NXBOX_D3D12_EXACT_CONDITIONAL=1` enables the corrected comparison. The GPU path
normalizes the two words to an 8-byte boolean with operation 3 of the existing
query resolve shader, then binds native nonzero predication. It retains the input
and output resources, invalidates overwritten graphics state, orders UAV writes,
and uses a fresh output allocation so earlier draws keep their original predicate.
It introduces no explicit CPU fence wait or download. The CPU predicate bisect
uses the same two-word rule. Two-report equality and known-zero optimizations
keep their existing whole-64-bit semantics.

The switch is **off by default** pending Xbox validation of the added dispatch.
The shader regression executes the actual HLSL body with all 16 combinations of
zero, one, high-bit and all-one words; it failed before the change. The recording
harness executes the production method with command mocks, nonzero buffer offsets,
resource-state checks, repeated predicates and overflow rejection. Existing query
reduction/reset tests continue to pass.

This defect alone cannot explain missing geometry: the previous single-condition
comparison admitted a superset of draws. It is not a confirmed Scarlet fix, does
not establish that Legends Z-A is unlocked, and has no measured fps improvement.
Do not remove synchronization based on `sync_wait` being the largest phase.
Run the baseline capture above first; then repeat with a fresh dump directory and
only `NXBOX_D3D12_EXACT_CONDITIONAL=1` added. If needed, a third run can add
`NXBOX_D3D12_CPU_PREDICATE=1` to compare CPU and normalized GPU decisions. Build,
DXIL compilation, console output and locked 60 fps remain unverified.
