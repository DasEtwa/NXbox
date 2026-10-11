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
