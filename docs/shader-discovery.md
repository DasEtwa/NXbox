# Shader discovery: M1 source audit and experiment contract

Status: the review fixes implement upload and sampled live-region discovery.
Host tests cover the scanner/runtime with ASan/UBSan and the real cache corpus.
Compute discovery remains unsupported. No console timing or speedup is claimed.

## What the source establishes

Pipelines can be reconstructed without retaining HLSL **if** the original guest
instructions can be recovered locally and every other compilation input can be
replayed. A shader hash identifies bytes; it does not supply them or describe an
environment specialization. Whether recovery can happen early enough is unknown.
Neither the existing disk format nor the source establishes when games upload
their shaders relative to the first draw.

The synchronous graphics path in
`src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp:1588` constructs graphics
environments, builds control-flow graphs, translates each guest program, merges
VertexA/VertexB when necessary, constructs RuntimeInfo, emits HLSL, then calls
GetGraphics. GetGraphics at line 669 compiles HLSL with DXC, constructs the root
signature, and creates the native PSO. Compute uses ComputeEnvironment and a CFG
starting directly at StartAddress (`ComputeTranslation`, line 824).

The asynchronous path already demonstrates the required ownership boundary:
GPU-thread environment snapshots and register copies are captured before worker
compilation (`d3d12_pipeline_cache.cpp:1495`). FrozenEnvironment owns code,
constant buffers, descriptors, and environment state
(`src/video_core/shader_environment.cpp:481`). This snapshot contains game code
and must remain local; it is not the proposed shared list.

## Recompiler environment: complete interface inventory

The authoritative interface is `src/shader_recompiler/environment.h:71`.
Record answers per environment variant, not just per shader hash. An unanswered
query must abort speculative compilation and fall back to the live path; zero is
a possible answer and must not mean missing.

| Query or exposed state | Required replay input and current source |
| --- | --- |
| ReadInstruction(address) | Locally recovered guest bytes, their origin, and sufficient bounds for every CFG read. GenericEnvironment can read beyond the initial linear scan. |
| ReadCbufValue(bank, offset) | Exact queried u32 answers, including out-of-range zero. Graphics reads the stage's enabled constant-buffer bindings; compute reads QMD bindings. No guest addresses are needed in the shared answer table. |
| ReadTextureType(raw_handle) | Exact Shader::TextureType answer per queried raw handle. Current code resolves TIC entries using header-index binding mode or the masked handle. |
| ReadTexturePixelFormat(raw_handle) | Exact Shader::TexturePixelFormat answer per queried raw handle. |
| IsTexturePixelFormatInteger(raw_handle) | Derived from the preceding pixel-format answer through the versioned surface-format classification. Record separately if the replay ABI cannot guarantee that derivation. |
| ReadViewportTransformState() | Graphics viewport_scale_offset_enabled; compute defaults to 1. |
| TextureBoundBuffer() | Graphics bindless_texture_const_buffer_slot or compute tex_cb_index. |
| LocalMemorySize() | Graphics SPH low/high local size plus CRS size; compute QMD local_pos_alloc + local_crs_alloc. |
| SharedMemorySize() | Compute QMD shared_alloc; graphics default zero. |
| WorkgroupSize() | Compute QMD block_dim_x/y/z; graphics default zeros. |
| HasHLEMacroState() | Graphics engine hint OnHLEMacro; compute false. Preserve this explicitly, even if there are no positive replacement answers. |
| GetReplaceConstBuffer(bank, offset) | Optional BaseVertex/BaseInstance/DrawID answer; preserve explicit null results as well as positive replacements. |
| SPH() | 0x50-byte graphics header read from the game's own GPU memory. Supplies stage attributes, interpolation/output masks, geometry settings and memory metadata. Do not publish the raw header; recover it with the code locally. Compute has no graphics SPH. |
| GpPassthroughMask() | Eight u32 post_vtg_shader_attrib_skip_mask values for graphics. |
| ShaderStage() | VertexA, VertexB, tessellation control/evaluation, geometry, fragment, or compute. VertexA-only emission and dual-vertex merging require the original guest slot identity. |
| StartAddress() | Program-relative entry offset, with graphics CFG entry at +0x50 and compute entry at +0. Relocate reads consistently when the receiving console uses another offset. |
| IsProprietaryDriver() | Currently TextureBoundBuffer() == 2; changes the hash scan's EXIT handling. |
| Dump(...) | Debug side effect, not a compilation input; speculative replay should not export code. |

Graphics implementations are at `shader_environment.cpp:313-425`; compute at
427-478. The public cbuf_word_cache, handle_cache and track_cache are translation
memoization, not independent guest inputs; reconstruct empty for each translation.
IR instruction pointers must never be serialized.

Sparse answer capture must observe the actual translation. Merely copying the
tables before translation would omit queries not yet asked. Texture handles and
constant-buffer values can differ for identical code between draws. Record a
separate variant when answers differ. Existing maps use emplace, so reusing one
environment across changing guest state is not a reliable recording strategy.

## RuntimeInfo and other specialization inputs

`GraphicsRuntime` (`d3d12_pipeline_cache.cpp:831-937`) reads or derives:

- force_early_z from mandated_early_z; input_topology from the guest draw topology.
- Fragment alpha-test function/reference when enabled and not Always.
- dual_source_blend from target-zero blend enable and the selected global or
  per-target blend factors.
- previous_stage_stores and previous_stage_legacy_stores_mapping from the prior
  translated stage; no prior stage means an all-set varying mask. Rebuild these
  from the ordered stage bundle, rather than from current draw registers.
- y_negate from viewport scale enable, scale_y sign, window-origin mode and Y
  swizzle, applied only to the final vertex stage (geometry if enabled, otherwise
  VertexB); convert_depth_mode from that stage and depth_mode.
- generic_input_types[32] from vertex attribute constant/type/size, including
  signed packed-10 normalization and scaled integer cases.
- color_output_types[8] from mapped render-target formats and integer/sign
  classification.
- Transform-feedback enable, controls and stream_out_layout are consulted, but
  the function currently throws for enabled guest transform feedback before
  its later varying construction block. Do not claim that block makes guest
  transform feedback supported.

All RuntimeInfo fields are declared in `src/shader_recompiler/runtime_info.h:89`.
The remaining fields (tess_primitive, tess_spacing, tess_clockwise,
fixed_state_point_size, glasm_use_storage_buffers, and ordinarily xfb state)
retain their default values in this D3D12 helper. A versioned schema should still
give them explicit defaults, so a future compiler change cannot reinterpret old
records. Compute currently emits with a default RuntimeInfo (see the compute
translation/emission call sites); its local/shared/workgroup inputs are in its
Environment.

RuntimeInfo is not the entire recipe. Preserve guest stage order, whether to merge
dual vertex programs, guest and native topology, framebuffer color presence
(the fragment stores_frag_color mask is reduced to bound slots before emission,
line 1621), and provoking-vertex state (flat quads can be rejected). Apply
ConvertLegacyToGeneric in the same order, and allocate backend bindings across
stages in the same order. Descriptor counts, Shader::Info binding metadata,
root-signature layout, input layout and any stream-output declaration must then
be regenerated from the local translation.

Host capability and compiler policy inputs must come from the receiving console,
not be trusted from a different device: TranslationState at lines 778-819 queries
float64/int64, waves and viewport/layer support and configures typeless image
loads, alignment, clip limits and descriptor policy. Version the emitter, DXC
options, binding ABI, format conversions, and relevant renderer settings/probes.
Current native graphics keys salt the HLSL hash with StageBindingAbi, root-CBV
choice, topology and optional transform-feedback state (lines 679-696).
Do not substitute the guest code hash for those native keys.

## Fixed pipeline state

`Pso::GraphicsPsoState` is at
`src/video_core/renderer_d3d12/d3d12_pso_tables.h:578`. Its complete fields are:

- topology_type, cull_mode, fill_mode, front_ccw; cull_all, depth_clip,
  depth_enable, depth_write; depth_func, stencil_enable, stencil_read_mask,
  stencil_write_mask.
- independent_blend, num_render_targets, sample_count, patch_vertices;
  logic_enable, logic_op, alpha_to_coverage, conservative_raster; smooth_lines,
  depth_bounds, stencil_back_read_mask, stencil_back_write_mask.
- depth_bias, slope_scaled_bias_bits, bias_clamp_bits; dsv_format and
  rtv_formats[8].
- front/back StencilFaceState: fail, depth_fail, pass, func.
- blend[8]: enable, write_mask, src, dst, op, src_alpha, dst_alpha, op_alpha.
- attributes[32]: enabled, slot, offset, dxgi, instanced, swap_rb, scaled, pad,
  divisor. Encode semantic fields explicitly and canonicalize pad to zero.

FillGraphicsState (`d3d12_pipeline_cache.cpp:530`) derives this from registers;
ApplyFramebufferFormats and ApplyVertexReads subsequently specialize it to actual
attachments and translated vertex reads. Preserve the final state used to build
the PSO and native topology. Do not serialize D3D12 descriptors containing host
pointers. Dynamic viewport/scissor, resource addresses, and blend/stencil
constants set at command recording time are not themselves a PSO recipe.
Register inputs that affect shader emission still belong in the runtime variant,
even when similarly named fixed or dynamic state also exists.

## What is on disk today

The D3D12 disk envelope is independent of upstream's FileEnvironment.
`src/video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h:31-57` defines identity
and record types; serialization is in the corresponding cpp at line 86.
The explicit little-endian envelope contains magic/version, emitter/DXC/schema
versions, vendor/device/subsystem/revision, adapter LUID, driver version, kind,
key, payload/request/runtime lengths, the three byte arrays and a checksum.
Lengths are bounded and corrupt/stale records miss. LUID is retained but ignored
for compatibility across boots; driver matching is required for native blobs and
libraries, while portable recipes/DXIL follow their own identity checks.

| Record | Contents |
| --- | --- |
| .dxil (Dxil) | Validated compiled shader bytecode, looked up by the existing DXC key. |
| .graphics (Graphics) | Recipe plus guest request_key and runtime_data. |
| .compute (Compute) | Recipe plus guest request_key and runtime_data. |
| .gpso / .cpso (NativeGraphics / NativeCompute) | Native driver cached PSO bytes. |
| pso.library (Library) | Native D3D12 pipeline library blob. |

Recipe (`d3d12_pipeline_cache.cpp:249-275`) writes recipe version 4, root-CBV
selection, native topology, packed graphics state when applicable, stage count,
then each stage's stage enum, hash, descriptor counts and **HLSL source bytes**.
It is not a code-free hash list. request_key is the packed GraphicsPipelineKey
guest request, not an Environment transcript. runtime_data stores translated
Shader::Info and optional transform-feedback metadata needed for draw binding
(`WriteInfo`/`WriteFeedback`, and replay at line 1191), not all register-derived
RuntimeInfo or recompiler query answers. Existing disk replay therefore does not
need the game's original GPU memory, but depends on retained generated code.

Upstream GenericEnvironment::Serialize (`shader_environment.cpp:219-267`) and
FileEnvironment::Deserialize (664-725) store:

- Code size, table counts, local memory, bound texture CBUF slot, start address,
  cached/read interval endpoints, viewport-transform state, stage, and **guest
  code bytes**.
- Texture type and pixel-format maps, CBUF value and replacement maps.
- Compute workgroup/shared memory, or graphics **SPH bytes** and geometry
  passthrough mask. initial_offset is reconstructed as 0 for compute or 0x50 for
  graphics; proprietary-driver status is reconstructed from bound slot == 2.

FileEnvironment is a useful replay-interface precedent, not a shareable no-code
format. It derives HasHLEMacroState from nonempty replacement maps, which is less
expressive than the live engine-hint flag. Its missing-answer behavior must not
be silently reused for speculative records. CanBeSerialized excludes environments
with instruction reads outside their cached code range.

## Guest address and exact hash semantics

`src/video_core/shader_cache.cpp:53-78`: graphics GPU address is
`maxwell3d.regs.program_region.Address() + regs.pipelines[guest_slot].offset`.
The base is a GPU virtual address; offset is a u32. Enabled stages and disabled
rasterization control which slots are used. Both vertex slots matter.

Compute (`shader_cache.cpp:87-99`) uses
`kepler_compute.regs.code_loc.Address() + launch_description.program_start`.
Compute has no graphics header; its QMD supplies metadata separately. Base
addresses belong to the channel's MemoryManager/address space. Never merge equal
GPU addresses from different address spaces.

Normal hash (`shader_environment.cpp:160-167,270-300`):

1. Scan from **start_address**, including the 0x50 graphics header, in 8-byte
   words. Read successive 0x1000 blocks, with a 0x100000-byte maximum.
2. Stop at the first exact u64 `0xE2400FFFFF87000F` or
   `0xE2400FFFFF07000F`; hash all bytes before that word, excluding the branch.
3. For nonproprietary driver only, also stop at exact
   `0xE30000000007000F`, **including** this EXIT word in the hash.
4. Hash that prefix with Common::CityHash64, without an additional seed. These
   are the guest unique_hash values. Pso::HashBytes is a different hash and must
   remain unchanged.

There is an important size asymmetry: Analyze hashes `size`, but
CachedSizeBytes returns `cached_highest - cached_lowest + 8`. Thus ShaderInfo's
size includes one extra word beyond the hashed prefix. For the branch case that
extra word is the excluded branch; for the EXIT case it is the word following
EXIT. A scanner that hashes ShaderInfo::size_bytes bytes will disagree.
Read availability for the extra word and full CFG range matters to later replay,
even though it need not be part of the linear hash.

If no terminator is found within 1 MiB, MakeShaderInfo
(`shader_cache.cpp:231-248`) builds a CFG and CalculateHash hashes the inclusive
minimum-to-maximum instruction-read interval, including gaps
(`shader_environment.cpp:193-198`). This interval can differ from the linear
start/prefix. A scanner that cannot reproduce CFG analysis must mark this case
unsupported and report it separately, not report proven absence.

ProgramHeader (`src/shader_recompiler/program_header.h:29-250`) is a packed
0x50-byte bitfield layout, not a structure with an unambiguous magic signature
and a code-length field. Header-like bytes alone are not evidence that a shader
was found. Alignments and accepted field combinations need validation against
first-use observations; this audit does not establish a safe exhaustive alignment
filter. A future shared list can provide exact hash lengths and header-relative
hash ranges and validate candidates against its hashes. In the graphics normal
case a matching hash also validates the header bytes because they are included.
For fallback hashes, validate the separately recovered header and full code range
locally as well. A code hash is not a cryptographic authenticity guarantee.

## Proposed code-free list schema

Use a bounded, explicitly encoded format with canonical integers/enums, lengths,
float bit patterns, content checksum and schema version. The following is a field
schema, not an implemented serializer. It intentionally has no arbitrary code
blob or raw-register-dump field.

```text
manifest:
  schema_version, title_id, game_build_id, guest_isa_version
  translator_environment_abi, runtime_abi, fixed_state_abi, binding_abi
  renderer_policy_id
  shaders[]:
    shader_id, guest_slot, guest_hash
    hash_algorithm: cityhash64
    hash_mode: linear_self_branch | linear_exit | cfg_read_interval
    hash_begin_from_program_start, hash_length, required_read_ranges[]
    graphics_header_required, proprietary_driver
    environment_variants[]:
      variant_id
      texture_bound_buffer, local_memory_size, shared_memory_size
      workgroup_size[3], viewport_transform_state, has_hle_macro_state
      gp_passthrough_mask[8]
      cbuf_answers[]: bank, offset, value_u32
      replacement_answers[]: bank, offset, present, replacement_enum
      texture_answers[]: raw_handle, texture_type, pixel_format
  pipelines[]:
    pipeline_id, kind
    stages[]: guest_slot, shader_id, environment_variant_id
    merge_dual_vertex, guest_topology, native_topology, provoking_vertex
    runtime_per_emitted_stage:
      generic_input_types[32], color_output_types[8]
      force_early_z, input_topology, alpha_test_present, alpha_test_func
      alpha_test_reference_bits, y_negate, convert_depth_mode, dual_source_blend
      tess_primitive, tess_spacing, tess_clockwise
      fixed_point_size_present, fixed_point_size_bits, glasm_use_storage_buffers
      xfb_count, xfb_varyings[]
    fragment_attachment_presence[8]
    fixed_graphics_state: explicit GraphicsPsoState semantic fields above
    priority: optional observed first-use frame/time
```

previous_stage_stores and legacy mapping are deterministically reconstructed from
the ordered local translations. SPH is read from local code, never copied into
the manifest. Rebase start addresses and instruction reads consistently; do not
carry producer GPU virtual addresses to another console. A pipeline may reference
multiple environment variants of the same shader hash. Record both absent and
present optional query answers. Reject missing answers, unexpected queries,
unsupported versions, unresolved stages and unsupported graphics features.

The format must reject guest code, SPH byte arrays, HLSL, DXIL, native PSO blobs,
resource payloads and host pointers. CBUF values are restricted to the u32 answers
actually requested for translation, not whole constant-buffer contents. Those
answers can still be numerous and build-specific; measure list size and review
the exporter rather than assuming a small or content-independent list.

## M2/M3 implementation and review fixes

`NXBOX_SHADER_DISCOVERY_TRACE=1` is latched once. With the flag absent or `0`,
hooks do not allocate, copy memory, inspect clocks or log. Reset allocates less
than 8 MiB, verified by static_assert. Graphics/compute first demand hooks precede
pipeline cache early returns. Demand means pipeline request, not completed draw.
Records use MemoryManager::GetID(), hash and kind, so address reuse in a different
space cannot inherit history. CFG-fallback hashes are marked unsupported and
excluded from never-seen bins. Compute also remains outside those bins.

The SPH predicate accepts sph_type 1 with stages 1–4 and sph_type 2 with stage 5,
version 3, and reserved bits 21–22 clear. Bit 25 is valid. The scanner retains
overlapping candidates, excludes self-branches from the hash, includes EXIT only
for nonproprietary mode, and continues to find the proprietary self-branch hash.
No cache key or disk format changes. A header is only a heuristic; a demand hash
match is required to interpret a discovery as relevant to a used shader.

Both scalar and bulk linear Upload::State paths copy bounded snapshots
synchronously, retaining no guest pointers. They cover Maxwell3D, KeplerCompute
and KeplerMemory users of the shared upload engine. Four immutable upload slots
hold at most 1 MiB each. Larger/overflowed uploads still extend live coverage;
the snapshot loss increments upload_overflow. Scalar/bulk event counters and
scalar_uploads_with_sph/bulk_uploads_with_sph identify the observed code path.
SPH counters describe captured, scanned uploads; losses are reported separately.
Candidates without a registered base increment candidates_before_base.

TickFrame runs the diagnostic before rasterizer cache locks. At most once per
250 ms, it drains scanner steps until done or a shared 2 MiB work / soft 1 ms CPU
budget is exhausted. Work includes header probes, instruction probes, hash bytes
and live-memory copies. Clock checks occur between candidates, every 4 KiB of
scanner probes, and every 64 KiB memory read. Roughly half the slice is reserved
for live scanning under upload pressure. A hash/read already in progress can
overshoot the soft CPU budget; the byte limit remains enforced. Upload capture
copies and buffered log writes are outside scan_ms and must be measured by the
trace-on/off console control too.

Base register writes select the active graphics program base per address space.
Historical/transient bases occupy the bounded 16-entry table but are not polled.
EndFrame uses only the currently bound MemoryManager pointer; no pointer is
retained across frames. It reads through ReadBlockUnsafe on the GPU command
thread, where existing shader-environment reads also run. Unlike ReadBlock,
this avoids rasterizer cache flushes; GPU-written data may therefore be stale.
The memory manager zero-fills unmapped pages; the scanner checks the GPU
address range as well.

Each active region starts with a bounded 16 MiB sweep, extended by observed
upload extents or demand offsets (at most the 32-bit program-offset range).
One reusable 2 MiB window is read in 64 KiB chunks, then held immutable until its
incremental scan finishes. Candidate starts are restricted to the first MiB;
the second MiB supplies the tail for candidates crossing window boundaries.
The next window starts one MiB later. Sweeps repeat while a base is active,
including when CPU writes produce no upload command. This is polling, not an
exhaustive allocation map or dirty-generation tracker. max_region_extent,
active_regions, live_read_bytes, live_windows and max_sweep_age_s expose coverage
and lag; an unseen shader beyond the current extent can be missed until demand
extends it. No frame tick means no scanning during that interval.

CPU writes can race a sampled copy, just as they can race existing environment
reads. A copied window is not a synchronized residency snapshot, and read chunks
may span multiple ticks. Matching hashes are diagnostic observations only; this
code never compiles, caches or publishes programs from a scan. Live timestamps
are taken after the final window read, deliberately conservative. Upload seen
uses capture time, even if scanning completes after first demand. Thus an early
upload can acquire positive lead time retroactively after its scan, but a late
upload cannot. seen_before/lead_* use the earliest observation of either source;
upload_seen_before and live_seen_before identify each source independently.
Bins are [0,0.1), [0.1,1), [1,10), [10,60], (60,infinity), and never seen before
use; timestamps must be strictly earlier than first demand.

Base/first-use/first-seen events enter a fixed 1,024-event buffer. Formatting and
Diagnostic (including file flushes) occur only at EndFrame or orderly Shutdown,
outside pipeline_mutex. event_overflow explicitly reports dropped detail lines;
aggregate records survive. Every 60 seconds and at shutdown summaries report
counts, leads, unsupported hashes, pending uploads, overflow, work and scan CPU.

### Host proof and console experiment

The review baseline passes 213 D3D12 tests. The new runtime tests use the actual
scanner, runtime and CityHash, replacing the logger and memory manager only.
They cover real SPH words, 300 shaders drained through one EndFrame slice,
capture-before-demand and capture-after-demand, bounded overlapping live reads,
unmapped zero tails, base switching, oversize uploads, unsupported CFG counts,
rate limits, source counters, disabled behavior and lifecycle under ASan/UBSan.
`shader_discovery_corpus_test.cpp` accepts the reviewer's extracted envs.bin and
checks Recompute against every recorded cache hash without storing guest code
in this repository. The reviewer harness is rerun independently too.

The lead must supply a CI-validated package; none was built here. Preserve other
lines in LocalState/nxbox_env.txt and set:

```text
NXBOX_SHADER_DISCOVERY_TRACE=1
NXBOX_D3D12_FRAME_PROFILE=1
```

Fully terminate/relaunch, select native D3D12, then run BotW and MK8D for five
minutes each on a reproducible route, with isolated cold and populated caches.
Optionally repeat Wonder/Odyssey. Gracefully exit; retain eden_uwp_diag.txt,
package ID, effective options, route and cache state. Check first_use on warm
and cold caches; nonzero live_read_bytes/live_windows; advancing full sweeps;
scalar/bulk upload counts versus their SPH counts; demand-matching observations;
early leads; pending uploads; unsupported and overflow counters. Repeat with
trace `0` and compare visuals/FPS and pipeline_lookup, shader_translation,
dxc_compile and pso profile phases. Report diagnostic overhead separately.
Zero matches with slow/incomplete coverage cannot reject prebuild. Positive
lead time does not establish that specialization/environment answers were also
available early. The code-free replay/export/scheduling work remains unimplemented.

## M2 experiment design and unresolved implementation questions

The original M1 acceptance criteria are retained below as historical design notes. The reviewed M2
implementation above does not satisfy the complete safe live-memory scan.

- Latch `NXBOX_SHADER_DISCOVERY_TRACE=1` once, before installing hooks. Disabled
  hooks must not allocate, lock, read guest memory, inspect the clock or format
  log strings. Do not change cache hashes or draw behavior.
- Record first demand at CurrentGraphicsPipeline immediately after successful
  RefreshStages and before every warm-cache/last-request early return; compute
  after ComputeShader and before guest_compute lookup. Hooking only MakeShaderInfo
  misses warm runs. Record original guest hashes, guest slot/kind, channel/address
  space, GPU address, frame and monotonic time. Call it first pipeline demand:
  async compilation may delay or skip the actual draw. Never label compile-worker
  start time as first use.
- Observe graphics program_region and compute code_loc when registers change,
  before any draw. A first-demand-only base hook biases observations late. Handle
  channel rebinding and memory-manager destruction; do not retain stale pointers.
  Count base-only loads and intervals before the first present as well.
- Use RasterizerD3D12::TickFrame before its buffer/texture locks as the frame-end
  hook. Keep scanning on the GPU thread, where GPU command processing cannot
  modify the upload halfway through a scan. Do not use raw pointers on a helper
  thread. This alone does **not** exclude guest CPU writes or mapping changes.
  Read through MemoryManager's synchronized read path and verify the memory
  invalidation/synchronization contract before claiming a stable snapshot.
  ReadBlock may flush GPU caches; bounded bytes do not bound flush latency.
- Discover mapped intervals for the current address space; do not treat
  program_region as a known allocation length. Restrict reads to mapped bytes.
  Invalidation DAddr intervals must be mapped to tracked GPU code regions; a
  global dirty-on-every-buffer-write flag would scan continuously in gameplay.
  Do not mark unseen shaders cached or change guest page protections merely to
  observe writes.
- At most one slice per 250 ms, only for a new base or changed code interval;
  continue unfinished slices without losing a write generation. Proposed initial
  caps: 256 KiB of copied/scanned code per slice, 1 ms cooperative CPU budget,
  16 tracked regions, 16,384 hash records, 8 MiB total retained state, no eviction
  that silently rewrites first-seen history. Log overflow, scanned bytes, scan
  duration and full-region coverage age. These are proposed caps, not measured
  console costs. Large shaders need a resumable, generation-validated candidate
  reader, not a full 1 MiB read for every aligned candidate.
- Validate the normal hash against actual first-use cache hashes. Log unsupported
  CFG hashes, incomplete candidates and alignment-filter misses separately.
  Keep scan observations independent of demand; never set first-seen to first-use
  merely because demand successfully read the shader.
- SPH scanning covers graphics only. Compute needs a separate plan: a code-free
  manifest containing hash ranges plus verified entry alignments, or bounded
  instruction-stream candidate enumeration. QMD is available at dispatch but that
  does not establish earlier residency. Until then, report compute as unobservable,
  not as zero lead time or proven absent.
- Every 60 s and at orderly title shutdown, emit per-kind first-use count,
  independently seen-before count and mutually exclusive bins: <0.1 s, 0.1-1 s,
  1-10 s, 10-60 s, >60 s, never seen before use. Also report unobservable/overflow
  counts outside the measured population. Exact boundaries must be documented.
  Shutdown should not depend on static destruction after logging teardown.

An independent scan observation is a lower bound on how long code was available,
not its upload time. Failed observations may reflect scanner coverage, late base
discovery, alignment filters or short residency. Missing-before-use does not by
itself disprove the prebuild idea. Positive code availability also does not show
that a pipeline's environment answers were available live; the shared variant
would have to supply them.

## Host validation required for M3

The M1 baseline command was
`python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'`:
211 tests passed, no pre-existing failures. That was the M1-only result. The implementation and sanitizer validation above
supersede the absence of scanner tests; there is still no Xbox build result.

M3 must compile the pure scanner with
`clang++ -std=c++20 -fsanitize=address,undefined`, linking the repository's real
`src/common/cityhash.cpp`. Test empty/truncated/unaligned buffers, SPH false
positives, all candidate boundaries, candidates spanning scan slices/pages,
terminator at 0/0xff8/0x1000/1-MiB boundary, both self-branches excluded, EXIT
included only for nonproprietary mode, the extra cached word excluded, multiple
headers, unsupported CFG fallback, address overflow, dirty-generation restart,
record/region capacity overflow, and missing/late/early/reused-address events.
Use independent expected ranges and fixed golden hashes; do not test the scanner
only against a copy of its own helper. No change to Pso::HashBytes is permitted.

## Console plan and decision rule (M4 planning only)

No CI/build/package was started. The lead must first integrate and validate M2/M3
and supply a package run ID. The console tester, not this worktree agent, runs:

1. BotW, MK8D, Wonder and Odyssey, same renderer/settings and reproducible route,
   five minutes each from launch, once with an isolated empty per-title D3D12
   cache and once with the cache populated by that route. Preserve existing
   cache data through the tester's normal backup/isolation method; do not delete
   game data. Restart the process between runs because the trace flag is latched.
2. Set `NXBOX_SHADER_DISCOVERY_TRACE=1` and
   `NXBOX_D3D12_FRAME_PROFILE=1` in the tester's launch environment. Save the
   effective environment with each run. Leave async-policy settings constant and
   record them; no evidence here establishes that raw shell assignments reach UWP.
3. Record the complete diagnostic output (`eden_uwp_diag.txt` in the tester's
   normal artifact directory), first-use and scan logs, 60-second summaries and
   final shutdown summary. Gracefully exit the title after five minutes.
4. For a control, repeat the same route/cache with trace unset. Compare FPS and
   GPU-thread phase timings, especially pipeline_lookup, shader_translation,
   dxc_compile and pso (the printed FramePhase names in
   `d3d12_context.cpp:489`). These scopes overlap; do not add them as independent
   time savings. Also compare scan duration/coverage, stalled or skipped draw counts,
   and visual output. Do not attribute trace overhead to the base renderer.

Interpret bins as a distribution of **observed lower-bound lead time**, separately
for graphics and observable compute. Good evidence is high coverage without
overflow, many expensive pipeline dependencies independently found seconds early,
and lead exceeding the measured local translation + DXC + PSO queue/build time.
All stages required by a pipeline must arrive before that pipeline's deadline;
per-shader coverage alone overstates the opportunity. Weight feasibility by
pipeline build time, not just shader count. A 0.1-second lead may be useless behind
a queue of several expensive PSOs; a ten-second lead may still be useful during
loading. Full-cache runs check instrumentation completeness but already avoid
this cold-cache mechanism and cannot prove cold-start prebuild speedup.

If results are favorable, the next implementation needs a recording Environment
decorator, a bounded/versioned code-free exporter/importer, address-space-aware
manifest matching (including compute), safe local code snapshots, dependency
bundles and priority scheduling, replay environments with strict missing-answer
fallback, and publication into existing cache maps under their existing ownership
rules. Reuse the existing worker pool and native cache APIs; preserve all existing
keys and synchronous fallback. Measure memory, peak outstanding compilations,
list size, discovery latency, CPU contention, DXC duration and PSO completion
deadlines before enabling it by default. This is a substantial second feature,
not merely exporting current recipes; implementation time cannot be responsibly
estimated from source inspection alone.
