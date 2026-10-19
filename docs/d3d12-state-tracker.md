# D3D12 state tracker register audit (M1)

Offsets below are **byte offsets** in `Maxwell3D::Regs`; divide by four for dirty-table method indices. Ranges are inclusive. Offsets and sizes were checked with a local C++20 `offsetof`/`sizeof` probe against the actual header (2026-10-19). Source references in this audit describe the unmodified baseline.

## FillGraphicsState

`src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp:530`

| Register storage | Inclusive byte range |
|---|---|
| `anti_alias_alpha_control` | `0x153C–0x153F` |
| `blend` | `0x133C–0x137F` |
| `blend_per_target` | `0x1E00–0x1EFF` |
| `blend_per_target_enabled` | `0x12E4–0x12E7` |
| `color_mask` | `0x1A00–0x1A1F` |
| `color_mask_common` | `0x0F90–0x0F93` |
| `conservative_raster_enable` | `0x1148–0x114B` |
| `depth_bias` | `0x15BC–0x15BF` |
| `depth_bias_clamp` | `0x187C–0x187F` |
| `depth_bounds_enable` | `0x19BC–0x19BF` |
| `depth_test_enable` | `0x12CC–0x12CF` |
| `depth_test_func` | `0x130C–0x130F` |
| `depth_write_enabled` | `0x12E8–0x12EB` |
| `gl_cull_face` | `0x1920–0x1923` |
| `gl_cull_test_enabled` | `0x1918–0x191B` |
| `gl_front_face` | `0x191C–0x191F` |
| `line_anti_alias_enable` | `0x1570–0x1573` |
| `logic_op` | `0x19C4–0x19CB` |
| `patch_vertices` | `0x0DCC–0x0DCF` |
| `polygon_mode_front` | `0x0DAC–0x0DAF` |
| `polygon_offset_fill_enable` | `0x0DC8–0x0DCB` |
| `polygon_offset_line_enable` | `0x0DC4–0x0DC7` |
| `polygon_offset_point_enable` | `0x0DC0–0x0DC3` |
| `rasterize_enable` | `0x037C–0x037F` |
| `slope_scale_depth_bias` | `0x156C–0x156F` |
| `stencil_back_func_mask` | `0x0F5C–0x0F5F` |
| `stencil_back_mask` | `0x0F58–0x0F5B` |
| `stencil_back_op` | `0x1598–0x15A7` |
| `stencil_back_ref` | `0x0F54–0x0F57` |
| `stencil_enable` | `0x1380–0x1383` |
| `stencil_front_func_mask` | `0x1398–0x139B` |
| `stencil_front_mask` | `0x139C–0x139F` |
| `stencil_front_op` | `0x1384–0x1393` |
| `stencil_front_ref` | `0x1394–0x1397` |
| `stencil_two_side_enable` | `0x1594–0x1597` |
| `transform_feedback_enabled` | `0x0744–0x0747` |
| `vertex_attrib_format` | `0x1160–0x11DF` |
| `vertex_stream_instances` | `0x1880–0x18FF` |
| `vertex_streams` | `0x1C00–0x1DFF` |
| `viewport_clip_control` | `0x193C–0x193F` |
| `window_origin` | `0x13AC–0x13AF` |
| `zeta_enable` | `0x1538–0x153B` |

## UnsupportedState

`src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp:149`

| Register storage | Inclusive byte range |
|---|---|
| `anti_alias_alpha_control` | `0x153C–0x153F` |
| `depth_bias` | `0x15BC–0x15BF` |
| `depth_bias_clamp` | `0x187C–0x187F` |
| `depth_bounds` | `0x0F9C–0x0FA3` |
| `depth_bounds_enable` | `0x19BC–0x19BF` |
| `gl_cull_face` | `0x1920–0x1923` |
| `gl_cull_test_enabled` | `0x1918–0x191B` |
| `iterated_blend` | `0x0DD0–0x0DD7` |
| `line_anti_alias_enable` | `0x1570–0x1573` |
| `line_stipple_enable` | `0x166C–0x166F` |
| `line_width_aliased` | `0x13B4–0x13B7` |
| `line_width_smooth` | `0x13B0–0x13B3` |
| `pipelines` | `0x2000–0x217F` |
| `point_size` | `0x1518–0x151B` |
| `polygon_mode_back` | `0x0DB0–0x0DB3` |
| `polygon_mode_front` | `0x0DAC–0x0DAF` |
| `rasterize_enable` | `0x037C–0x037F` |
| `slope_scale_depth_bias` | `0x156C–0x156F` |
| `transform_feedback` | `0x0380–0x073F` |
| `transform_feedback_enabled` | `0x0744–0x0747` |
| `viewport_scale_offset_enabled` | `0x192C–0x192F` |
| `viewport_transform` | `0x0A00–0x0BFF` |

## Specialization hash

`src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp:1373`

| Register storage | Inclusive byte range |
|---|---|
| `alpha_test_enabled` | `0x12EC–0x12EF` |
| `alpha_test_func` | `0x1314–0x1317` |
| `alpha_test_ref` | `0x1310–0x1313` |
| `depth_mode` | `0x0D7C–0x0D7F` |
| `mandated_early_z` | `0x0210–0x0213` |
| `provoking_vertex` | `0x1684–0x1687` |
| `stream_out_layout` | `0x2800–0x29FF` |
| `transform_feedback` | `0x0380–0x073F` |
| `transform_feedback_enabled` | `0x0744–0x0747` |
| `vertex_attrib_format` | `0x1160–0x11DF` |
| `viewport_scale_offset_enabled` | `0x192C–0x192F` |
| `viewport_transform` | `0x0A00–0x0BFF` |
| `window_origin` | `0x13AC–0x13AF` |

## MakeViewport / MakeFullViewport / MakeScissor

`src/video_core/renderer_d3d12/d3d12_rasterizer.cpp:167`

| Register storage | Inclusive byte range |
|---|---|
| `depth_mode` | `0x0D7C–0x0D7F` |
| `scissor_test` | `0x0E00–0x0EFF` |
| `surface_clip` | `0x0FF4–0x0FFB` |
| `viewport_scale_offset_enabled` | `0x192C–0x192F` |
| `viewport_transform` | `0x0A00–0x0BFF` |
| `window_origin` | `0x13AC–0x13AF` |

## Stencil / blend color / depth bounds

`src/video_core/renderer_d3d12/d3d12_rasterizer.cpp:1889`

| Register storage | Inclusive byte range |
|---|---|
| `blend_color` | `0x131C–0x132B` |
| `depth_bounds` | `0x0F9C–0x0FA3` |
| `gl_cull_face` | `0x1920–0x1923` |
| `gl_cull_test_enabled` | `0x1918–0x191B` |
| `stencil_back_func_mask` | `0x0F5C–0x0F5F` |
| `stencil_back_mask` | `0x0F58–0x0F5B` |
| `stencil_back_ref` | `0x0F54–0x0F57` |
| `stencil_enable` | `0x1380–0x1383` |
| `stencil_front_func_mask` | `0x1398–0x139B` |
| `stencil_front_mask` | `0x139C–0x139F` |
| `stencil_front_ref` | `0x1394–0x1397` |
| `stencil_two_side_enable` | `0x1594–0x1597` |
| `zeta_enable` | `0x1538–0x153B` |

## Exact subfields and conservative registration

- `FillGraphicsState` reads only `.frequency` from each vertex stream: `0x1C0C + 0x10*i` through `+3`, `i=0..31`; it reads all 32 instance flags and attribute words. The other stream words must retain both common vertex-buffer flags.
- `UnsupportedState` reads tessellation pipeline `.enable` at `0x2080` and `0x20C0` (`IsShaderConfigEnabled`, `maxwell_3d.h:2526`); these two raw enable bits must be compared explicitly: the common vertex-stream-limit registration also reaches these offsets and already occupies the second slot alongside Shaders.
- Transform feedback controls are `0x0700–0x073F`. UnsupportedState reads stream and varying_count (first two words per 16-byte control); specialization hashes all 64 bytes including stride/padding. It does not hash buffer addresses.
- UnsupportedState examines scale_x and x/y/z/w swizzles in all 16 viewport transforms. Specialization uses only viewport 0's scale_y sign and complete swizzle word. MakeViewport reads scale_x/y/z, translate_x/y/z and swizzle.y for each viewport; the complete `0x0A00–0x0BFF` array can conservatively be registered.
- MakeFullViewport reads both packed surface_clip words and window_origin.mode. MakeScissor reads all 16 enable/min/max records, viewport_scale_offset_enabled, both surface_clip words and window_origin.mode; render_area width/height then clamp its result.
- Stencil reference/back-only/IndependentStencil reads zeta_enable, stencil_enable, stencil_two_side_enable, gl_cull_test_enabled, gl_cull_face, both refs, both function masks and both write masks. The PSO also reads both face operation records. Blend color reads r/g/b/a; replication additionally depends on the raw attachment PSO and bound formats. Depth bounds reads both float words, clamped exactly as before, and raw attachment_state.depth_bounds.
- `surface_clip` already has a RenderTargets flag in table 0; table 1 remains available. `zeta_enable` already has ZetaBuffer/RenderTargets in both tables: preserve both, compare its raw value explicitly in cached inputs.

## Non-register dependencies

`d3d12_pso_tables.h:622` ApplyFramebufferFormats reads all eight colors, depth and sample_count; zero depth disables depth/stencil/bounds. Feature checks (`d3d12_pipeline_cache.cpp:1289–1350`) read the completed PSO, bound formats and immutable device capabilities/query failures. FillGraphicsState and UnsupportedState also read draw_manager.draw_state.topology. Specialization hashes converted PrimitiveTopology (including patch_vertices), and the request includes all six refreshed guest shader hashes. Preserve all disk hashes and specialization hash byte order.

`d3d12_rasterizer.cpp:630` independently rebuilds the raw attachment PSO (before pipeline feature normalization); its semantics must remain distinct from the pipeline request. Dynamic stencil uses that raw state and formats.depth; blend uses ConstantBlendFactors(raw state); bounds uses raw state.depth_bounds. Root constants read the pipeline object, render_area width/height and y_flip, then draw-state base_index/vertex_buffer.first, base_instance and indirect_draw_id (`:1885–1888`). Those per-draw words cannot be inferred from register dirtiness. Commands/list resets, channel changes and resolution rescaling must invalidate computed caches as well as recorded API state.

## Common-table preservation check

A standalone ASan/UBSan executable linked the real `dirty_flags.cpp` and `d3d12_state_tracker.cpp`, registered all ranges, and compared every pre-existing nonzero slot with the original common tables: **585 native register words registered; all common entries preserved**. This caught the overlapping common vertex-stream-limit registrations extending into `pipelines`: do not replace those entries or register the entire pipeline array. Compare tessellation enable bits explicitly, alongside zeta_enable, in every cache input.

## Expected cost reduction and limits (M3/M4)

On a pipeline hit, M3 removes UnsupportedState, the zero/8-target/32-attribute PSO construction, feature normalization and the unchanged specialization hashing chain; synchronization and RefreshStages, the existing pipeline mutex and the final key comparison remain. The supplied MK8D profile gives 5.60 ms / 1045 = **5.36 microseconds per draw** for the entire pipeline_lookup phase; this is an upper bound, not a predicted or measured saving. Expected average saving is the hit fraction times the avoided computation cost, minus the small fixed input checks. CityHash64 replaces only the in-memory hasher; FASTPATH=0 also restores its FNV-1a implementation for a complete A/B.

On a dynamic hit, M4 removes all **16 viewport and 16 scissor constructions**, stencil selection/IndependentStencil, blend factor selection, depth clamping and base root constant construction. It also caches the rasterizer's second raw FillGraphicsState/ApplyFramebufferFormats (charged to graphics_state). The supplied draw_recording phase is 5.45 ms / 1045 = **5.22 microseconds per draw** for the whole phase, so dynamic savings are an unknown subset of that, with the raw-state saving reported separately in graphics_state. No console saving or hit rate has been measured. A single conservative generation means a blend/scissor/stencil-only register write also invalidates the PSO request: correctness is favored over maximum hit rate. Splitting flag groups is a later experiment if the measured saving is small.

The slow calculations and hash statement order remain the original ones. Every consumer retains its own generation/input snapshot; consuming dirty flags for one does not make another clean. Both common slots remain intact, with zeta_enable and the two tessellation enables compared directly. Command-state resets set a pending invalidation, expanded to the native dirty bit when the next engine is observed (no dangling channel pointer). Rescale flags and changed render-area dimensions advance the same generation. Actual IA converted topology and the pipeline pointer are additional dynamic inputs; per-draw root constant words 8/9/10 are always refreshed. A null/pending pipeline pointer still goes through guest cache/async Lookup on every draw. Verify mode computes the original path on every eligible draw, compares byte representations on a would-be hit, and always uses the slow values, with at most 32 distinct diagnostic records.

## Tester steps

After the lead builds/packages the uncommitted work, edit the console's **LocalState\nxbox_env.txt**, preserving unrelated options. Fully terminate and relaunch NXbox between configurations: both mode switches are process-latched.

1. Set `NXBOX_D3D12_STATE_FASTPATH=1` and `NXBOX_D3D12_STATE_VERIFY=1`. Run MK8D (race, menu transitions), Odyssey (gameplay and area transitions), and BotW (gameplay, menus and loading). Include resolution changes, indexed/non-indexed draws, framebuffer changes and command-list flushes. `eden_uwp_diag.txt` must show startup `D3D12 state fastpath=1 verify=1`, **zero** `D3D12_STATE_VERIFY mismatch` lines, and unchanged rendering. Any mismatch blocks acceptance; preserve the complete diagnostic file and the scene/configuration.
2. Set `NXBOX_D3D12_STATE_VERIFY=0`, `NXBOX_D3D12_FRAME_PROFILE=1`, and `NXBOX_D3D12_STATE_FASTPATH=1`. Relaunch and capture the same repeatable warmed-cache scene for each game.
3. Change only `NXBOX_D3D12_STATE_FASTPATH=0`, relaunch, repeat the same scene, then repeat enabled again to check run-to-run variance. Compare `pipeline_lookup`, `draw_recording`, `graphics_state`, draw count, GPU-thread CPU/frame and FPS in `D3D12_FRAME_PROFILE`. Verification must be off for timing comparisons. Startup must report the requested modes.
4. Leave all unrelated bisection switches, resolution, async settings, shader caches, batching and flush cadence unchanged; do not clear player pipeline caches. Acceptance requires no mismatch and a reproducible improvement without visual regressions. CI/package run IDs are pending the lead; none were launched from this worktree.
