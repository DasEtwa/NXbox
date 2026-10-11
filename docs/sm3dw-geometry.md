# Super Mario 3D World geometry investigation

Baseline: `ecbaba6f21`, `/tmp/sm3dw_geo/diag_fix1.txt` and `fix1/shot-170s.png`.
The screenshot has displaced/occluding polygons as well as stipple and banding. Zero
skip/link/CBV counters exclude those particular failures, not shader or fetch correctness.
The pristine Mesa checkout remains unchanged. No console candidate was built or deployed.

## Ranked findings

1. **Disabled shader clip distances: concrete missing lowering, fixed here.** Eden applies
   the guest enable mask in `src/video_core/renderer_opengl/gl_rasterizer.cpp:944`.
   Pinned Mesa captures it in `src/mesa/state_tracker/st_atom_rasterizer.c:309`, but its
   `d3d12_compiler.cpp:133` final-stage lowering neither uses nor keys it. DXIL still exports
   `SV_ClipDistance` (`src/microsoft/compiler/dxil_signature.c:686`). Negative disabled
   distances can therefore clip geometry despite baseline `clip=0`.
   [Microsoft's semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics)
   define the clipping behavior. `patch_clip_distance` invokes Mesa's existing
   `nir_lower_clip_disable` only for the final VS/TES/GS; the 8-bit enable mask enters
   `common_all`, already compared for cache equality, and the common word enters the hash.
   This is a plausible scene cause, not a demonstrated console diagnosis.
2. **Vertex formats: no additional arithmetic defect found.** Packed SNORM clamps to -1;
   SSCALED/USCALED must remain **unclamped**. Exhaustive packed-lane and 8/16-bit scaled
   tests pass. RGB integer widening preserves RGB and replaces alpha with integer 1.
   Remaining GPU risks include RGB fetch overread at the resource end and signature/IA
   interpretation; host arithmetic mocks cannot establish their behavior on Xbox.
3. **Instancing: forwarding looks correct; unusual slot layouts need console evidence.**
   `d3d12_context.cpp:157` maps the divisor directly to `InstanceDataStepRate`;
   `d3d12_draw.cpp:1255` forwards base instance, and `:389` supplies the shader draw parameter.
   `u_vbuf.c:1142` applies base instance as an element offset, consistent with the
   separate divisor. No evidence yet warrants changing this path.
4. **Stencil: real capability limitations, not proved active here.**
   `d3d12_context.cpp:492` substitutes front read/write masks for back masks when OPTIONS14
   lacks support; `d3d12_draw.cpp:1110` uses separate references only with OPTIONS14 and
   command-list 8. Otherwise the front reference is used for both faces. The old `stencil`
   count merely says stencil is enabled. Requested read/write mask differences are retained in the diagnostic state before
   they become indistinguishable in the fallback descriptor. `stencil_mask_split` and
   `stencil_mask_unsupported` expose their use per frame.
5. **Independent/dual-source blend: weaker evidence.**
   `d3d12_context.cpp:361` copies per-target state and `:390` detects dual source.
   Baseline `dual=0` weakens the dual-source hypothesis. Independent blend enabled on one
   attached target is not evidence of an MRT error; the new `independent_mrt` separates it.

Ordinary GL depth clamp maps both near/far clipping off to `DepthClipEnable=false`
(`d3d12_context.cpp:597`), consistent with
[Microsoft's rasterizer contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc).
Asymmetric near/far clipping is not represented by that single flag. Depth bounds creation is
compiled out (`d3d12_context.cpp:470`), so `depth_bounds=0` cannot establish requested GL state.
The patch does not change either behavior.

All Mesa paths/lines above refer to pristine `/tmp/mesa-pin`, revision `15acdd7ea2b9`.

## Complete emulated format inventory

Pinned `d3d12_format.c:524` contains exactly these 15 formats:

| Source formats (PIPE_FORMAT_ prefix omitted) | IA format / conversion |
| --- | --- |
| R10G10B10A2_SNORM, R10G10B10A2_SSCALED, R10G10B10A2_USCALED | R32_UINT; signed extract then normalize/clamp for SNORM, signed/unsigned numeric conversion for scaled |
| B10G10R10A2_UNORM, B10G10R10A2_SNORM, B10G10R10A2_SSCALED, B10G10R10A2_USCALED | R32_UINT; same arithmetic, red/blue lanes reversed |
| R8G8B8_SINT, R8G8B8_UINT, R16G16B16_SINT, R16G16B16_UINT | Matching RGBA integer IA; RGB unchanged, alpha = integer 1 |
| R8G8B8A8_SSCALED, R16G16B16A16_SSCALED | Matching RGBA SINT IA; i2f32, no clamp |
| R8G8B8A8_USCALED, R16G16B16A16_USCALED | Matching RGBA UINT IA; u2f32, no clamp |

REV RGBA bit offsets are 0/10/20/30; BGRA offsets are 20/10/0/30. Alpha is two bits.
R10G10B10A2_UNORM is native, not in this inventory. BGRA8 UNORM and half R/RG/RGBA
are native when IA support is reported; otherwise u_vbuf translates. RGB half uses the
R32G32B32_FLOAT fallback (`u_vbuf.c:221`). All 65,536 half codes are checked against
an independent exponent/mantissa reference, including zeros, subnormals, infinities and NaNs.
This verifies Mesa's host fallback decoder, not the GPU's native half fetch.

## Console check

Enable `NXBOX_DRAW_DIAG=1` and repeat the same camera scene. Preserve per-frame
`fmt_*` counts and `clip_written` / `clip_disabled_written`. Nonzero disabled-written
counts establish that the new lowering applies to actual rasterized shader state; visual
improvement still needs screenshot comparison. Zero counts reject this explanation for
those frames. The counter describes original shader outputs, so it remains nonzero after
successful masking. Also inspect `instance_attr`, `divisor_gt1`, `base_instance`,
`instance_slot_conflict`, `depth_clip_asymmetric`, `frag_depth`, `stencil_ref_split`,
`stencil_ref_unsupported`, `stencil_mask_split`, `stencil_mask_unsupported` and `independent_mrt`.

## Host validation

`cd tests/port && python3 -m unittest discover -s . -p 'test_*.py'` completed with
178 tests, `OK`, process exit code **0**. Coverage includes the complete patch chain,
exactly-once `patch()` invocation, unchanged 32-bit common key storage, all 256 clip
masks with partial vector writes, requested stencil masks, the generated observer,
packed lanes, scaled integers, RGB alpha replacement and all half codes.
`git diff --check` passed; pristine Mesa still has no changes. Xbox visual validation
remains outstanding. No commit or push was made.

## Time-dependent corruption: upload readiness

The fix3 captures become clean late in the session. This weakens explanations based on
permanent vertex-format arithmetic, but does not by itself establish a readiness race.
The strongest concrete defect found here is **incomplete publication of CPU-converted
texture data**, rather than an unjoined ASTC worker:

- `src/video_core/renderer_opengl/gl_texture_cache.cpp:280` selects the decode worker
  only for `CpuAsynchronous`. The default `Cpu` path calls `ConvertImage` inline, and
  `src/video_core/textures/astc.cpp:2037` joins its internal row workers before returning.
  The asynchronous path queues at `texture_cache.h:1381`, returns without uploading,
  and uploads completed work in `TickAsyncDecode`. It can bind an image still decoding.
  Global and per-title settings can override the default; `UPLOAD_POLICY` now records
  the actual values. `NXBOX_ASTC_SYNC=1` forces `Cpu` after both settings files and before
  renderer construction (`src/eden_uwp/game_session.cpp:815`).
- `src/video_core/renderer_opengl/gl_shader_cache.cpp:504` returns no pipeline for
  unfinished asynchronous non-quad draws; it does not draw a placeholder shader.
  `gl_graphics_pipeline.cpp:441` waits for unfinished programs before binding them.
  Async shaders default to false, but settings files can override this too.
- Upload staging uses persistent, **noncoherent**, explicitly flushed maps
  (`gl_staging_buffer_pool.h:99`). `gl_texture_cache.cpp:849` previously flushed only
  `unswizzled_size_bytes`, the compressed source size. CPU ASTC conversion produces
  larger RGBA output, including mipmaps/layers, so part of the sampled output was outside
  the published range. It now flushes `MapSizeBytes(*this)`, the same size requested from
  the pool. Host tests execute this exact flush with a three-mip, two-layer ASTC example
  (10,752 compressed bytes versus 43,008 output bytes), ordinary and recompressed images.
- Pool reuse already rejects unsignaled fences (`gl_staging_buffer_pool.cpp:92`).
  Mesa temporary copies retain source/destination BO references before recording
  (`/tmp/mesa-pin/src/gallium/drivers/d3d12/d3d12_resource.cpp:1334`), and batch reset waits
  before releasing them (`d3d12_batch.cpp:156`). No early temporary upload reuse was found.
  `patch_upload_ready` instruments unexpected busy temporary maps and waits before CPU
  writes when diagnostics are enabled. Normal execution adds no busy-resource scan.
- `buffer_cache.h:124` invalidates CPU-written ranges; `:1758` uploads dirty ranges while
  excluding GPU-owned ranges. OpenGL uses immediate uploads, not mapped uploads
  (`gl_buffer_cache.h:264`). No guest read-before-write defect was established. Counts of
  invalidations and uploads expose activity, but cannot prove guest write ordering or
  detect every missing invalidation.

Enable `NXBOX_UPLOAD_DIAG=1` alongside `NXBOX_ASTC_SYNC=1` for the repeat capture.
`NXBOX_GL_UPLOAD_READY` and `NXBOX_MESA_UPLOAD_READY` are cumulative, sampled reports
collected by the frontend. They contain `astc_queued`, `astc_uploaded`,
`astc_pending_binding` (prepared graphics/compute texture bindings, not skipped draws),
`pipeline_pending`, `pipeline_skipped`, `staging_busy_avoided` (safe pool rejection),
`staging_busy_map` (unexpected busy Mesa temporary allocation), `buffer_invalidation`,
`buffer_upload`, and `expanded_flush` (uploads the old compressed-size flush would
under-publish). A pending pipeline can subsequently be waited on; it is not itself an
unsafe draw. Keep early and late captures: neither host checks nor the old zero skip/link
counts establish that this fix removes the Xbox artifacts.

Readiness validation: the requested `cd tests/port && python3 -m unittest discover -s .
-p 'test_*.py'` completed with **182 tests, OK, process exit code 0** on the final
source state. The full patch-chain LF/CRLF checks and exactly-once new-step assertion
passed. `git diff --check` passed and pristine `/tmp/mesa-pin` remained unchanged.
No Xbox candidate was built/deployed, and no commit or push was performed. The Mesa
report is published when an unexpected busy temporary map is observed; its absence
alone does not demonstrate that the instrumentation ran.
