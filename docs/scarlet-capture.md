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
