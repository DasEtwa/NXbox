# Native D3D12 backend

## Scope and selection

NXbox currently uses Eden OpenGL (SPIR-V shaders by default), Mesa Gallium d3d12,
then D3D12. The native backend removes the OpenGL state machine and Gallium driver
from this path. It retains Eden's Maxwell decoding, IR, SPIR-V emitter and
VideoCommon cache algorithms. Vulkan is the architectural reference, not a runtime
dependency of the native renderer. Existing Vulkan sources remain part of Eden's build.

P1 lives in `src/video_core/renderer_d3d12/`. It is compiled only when `WIN32` and
`CMAKE_SYSTEM_NAME=WindowsStore`. OpenGL remains the default. Select either:

```ini
# LocalState/nxbox_env.txt (also accepts a process environment variable)
NXBOX_RENDERER=d3d12
```

or:

```ini
# LocalState/eden_settings.txt
renderer_backend=5
```

Eden's canonical `backend=5` label also works; `renderer_backend` is an NXbox alias.
`Settings::RendererBackend::D3D12` is appended as value 5; values 0–4 are unchanged.
The environment override is case-insensitive and takes precedence over settings.
Unknown environment values preserve the configured backend. Restart after changing
it. Set this globally: a per-title setting which changes graphics API after window
creation is rejected before core loading. A per-title setting can retain the
already selected API. Other frontends do not instantiate this UWP-only backend.

The frontend still uses the `MesaWindow` EmuWindow adapter, but its native branch
returns a no-op `GraphicsContext` and does not load Mesa, DXIL or WGL. The library
and launch screen release their Direct2D swapchain before handing over CoreWindow.
Native launch-frame uploads are TODO(P2); they must use the renderer's swapchain.

P1 creates a hardware D3D12 device at feature level 11_0, direct queue, event-backed
fence, allocator/list, two RTVs and an opaque RGBA8 flip-discard CoreWindow swapchain.
It clears to `(0.08, 0.25, 0.50, 1.0)` once during initialization and on each composite,
transitions PRESENT → RENDER_TARGET → PRESENT, submits and presents with vsync.
Frames are serialized by fence completion before allocator reuse. Layout changes
retire work before releasing buffers and calling ResizeBuffers. Fence waits are
bounded at 10 seconds and inspect removal status. Runtime presentation failure
latches recording off and publishes `NXBOX_D3D12_COMMAND_FAILURE` for the existing
frontend shutdown path. Teardown drains the queue, including partial construction.

Guest draws, clears, compute, host caches and pipelines are stubs. First graphics,
compute and framebuffer use log warnings; the test clear is not guest output.
DMA/surface acceleration declines requests so engine CPU fallbacks can run; inline
writes and payload/timestamp queries retain the null renderer's CPU behavior.
Guest fence callbacks complete immediately because no guest GPU work is submitted.
Applet captures and screenshots return blank buffers. TODO(P2–P5) marks the native
rasterizer replacement points. No guest texture exists to copy in P1: guessing a
resource from a guest address would not constitute a usable stub cache.

## Vulkan to D3D12 ownership map

| Vulkan reference | Native D3D12 equivalent and responsibility |
| --- | --- |
| `vulkan_common/vulkan_device.*`, `VkDevice` | Device owns `ID3D12Device`, adapter capabilities, format/heap support and device-removal diagnostics. Probe shader model independently of feature level. |
| `vk_scheduler.*`, queue, command buffers/pools | Direct queue initially; command lists plus allocators. Allocators, descriptors, upload slices and resources retire against a monotonically increasing fence value. Never reset an allocator while submitted work references it. Add copy/compute queues only with explicit cross-queue waits. |
| Submission semaphores and scheduler ticks | Queue `Signal(ID3D12Fence, value)`, GPU queue waits and bounded CPU event waits. CPU completion, submission and guest syncpoints are separate concepts. |
| `vulkan_memory_allocator.*`, buffers/images | DEFAULT, UPLOAD and READBACK resources. Start with committed resources, later pooled placed resources. Respect `GetCopyableFootprints`, texture row/placement alignment and CBV 256-byte alignment; track budget within Xbox's shared process limit. |
| Descriptor sets/pools/update templates, `vk_update_descriptor.*` | Shader-visible CBV/SRV/UAV heap and separate sampler heap, plus CPU-only RTV/DSV heaps. Descriptor tables point to contiguous ranges; null descriptors and per-submission lifetime are required. No overwriting descriptors still used by the GPU. |
| Pipeline layout and push constants | Versioned root signature describes descriptor ranges/register spaces and root constants. Cache by binding schema, not by an opaque Vulkan layout handle. Root constants consume the shared 64-DWORD root budget; keep large blocks in CBVs. |
| `vk_pipeline_cache.*`, graphics/compute pipelines | Graphics/compute PSOs combine validated DXIL, root signature and fixed state. Include vertex layout, topology type, blend/raster/depth state, attachment formats and samples in the graphics key. Dynamic viewport/scissor, stencil reference and topology are set on the command list. |
| Image layouts and access/stage barriers | Per-subresource D3D12 resource-state tracker. Emit transition, UAV and aliasing barriers as applicable. Vulkan stage masks cannot simply be copied; distinguish ordered access from state transitions. Texture feedback must use legal nonoverlapping views or explicit copies. |
| Render-pass/framebuffer cache | RTV/DSV views plus `OMSetRenderTargets`; explicit load clears, store/discard policy and resolves. End incompatible attachment usage before sampling/copying. Render-pass APIs are optional later; no Vulkan render-pass dependency. |
| `vk_swapchain.*`, WSI and present passes | `IDXGIFactory2::CreateSwapChainForCoreWindow` receives the direct queue and CoreWindow ABI `IUnknown`, never HWND. `IDXGISwapChain3` selects the actual backbuffer index. Single owner per CoreWindow, flip model, opaque alpha, PRESENT state before DXGI Present. |
| Present layers/window adaptation | Native compositor will handle guest block-linear framebuffer lookup, crop, transform, format, multiple layers and scaling before writing the swapchain. P1 only clears it. |
| Queries, conditional rendering, transform feedback | Query heaps plus ResolveQueryData/readback, predication where equivalent, and stream-output buffers/counters. Guest observable results and fence ordering must remain correct, even with software fallback. |

## VideoCommon template contracts

These are compile-time duck-typed interfaces, not abstract C++ runtime base classes.
Implement D3D12 resource wrappers and parameter structs, then instantiate the same
templates. Do not alias Vulkan resource handles or advertise an unsupported trait.
The file:line anchors below refer to the current Eden reference implementation;
method overloads and `if constexpr` branches determine the precise required set.

| Contract | Declaration / reference | Required behavior |
| --- | --- | --- |
| `TextureCache<P>` | `src/video_core/texture_cache/texture_cache_base.h:131`, constructor `:159`; Vulkan traits `src/video_core/renderer_vulkan/vk_texture_cache.h:581` | P supplies Runtime, Image, ImageAlloc, ImageView, Sampler, Framebuffer, AsyncBuffer, BufferType and capability flags (validation, framebuffer blits, emulated copies, memory reporting, async downloads, MSAA downloads). Cache owns guest identity, overlap/alias tracking, invalidation, swizzling, format conversion and collection. |
| `TextureCacheRuntime` | `src/video_core/renderer_vulkan/vk_texture_cache.h:43` | Finish/TickFrame/FlushDeferredClear; UploadStagingBuffer/DownloadStagingBuffer/FreeDeferredStagingBuffer; GetDeviceLocalMemory/GetDeviceMemoryUsage/CanReportMemoryUsage; BlitImage, CopyImage/CopyImageMSAA, ShouldReinterpret/ReinterpretImage, ConvertImage; upload/MSAA capability queries, AccelerateImageUpload, InsertUploadMemoryBarrier, TransitionImageLayout, HasBrokenTextureViewFormats/HasNativeBgr, BarrierFeedbackLoop. Disabled traits avoid BlitFramebuffer/EmulateCopyImage paths. Optional NXbox budget/ASTC hooks are detected with `requires`. |
| Texture resource wrappers | `src/video_core/renderer_vulkan/vk_texture_cache.h:193` (Framebuffer), `:308` (Image), `:399` (ImageView), `:503` (ImageAlloc), `:525` (Sampler) | Image derives ImageBase and implements staging upload/download, scale and modification hooks. Views derive ImageViewBase and expose host views without losing guest metadata. Provide null-image/view constructors, framebuffer attachment metadata and sampler translation. Fence-retire all host views/resources. |
| `BufferCache<P>` | `src/video_core/buffer_cache/buffer_cache_base.h:173`; Vulkan traits `src/video_core/renderer_vulkan/vk_buffer_cache.h:272` | Runtime, Buffer, Async_Buffer, MemoryTracker; IS_OPENGL=false. Select persistent UBO binding, primitive support, binding indices, memory-map/upload and separate image-buffer flags honestly. Preserve dirty guest ranges, download scheduling and channel state. Sparse multi-range hooks are optional, so start with ordinary buffers. |
| `BufferCacheRuntime` | `src/video_core/renderer_vulkan/vk_buffer_cache.h:98` | TickFrame(slot_buffers), CurrentTick/IsFree/Wait/Finish; memory reporting; uniform/storage alignment; staging allocation/free; CanReorderUpload, PreCopyBarrier/CopyBuffer/PostCopyBarrier/ClearBuffer; index/quad/vertex/transform-feedback bindings; BindMappedUniformBuffer, BindUniformBuffer/BindStorageBuffer/BindTextureBuffer. With Vulkan-style flags, compute uses the same binding methods. Optional storage-limit/multi-range extensions can follow later. |
| Buffer / staging wrappers | `src/video_core/renderer_vulkan/vk_buffer_cache.h:34`; `src/video_core/renderer_vulkan/vk_staging_buffer_pool.h:22` | BufferBase metadata, null constructor, handle/view, mapped access, range-use/write ticks and CPU upload/download operations. Async staging carries resource, offset, mapped span and retirement identity. Replace VkBuffer/device addresses with native resource and GPU virtual address types. |
| `QueryCacheBase<Traits>` / `QueryCacheRuntime` | `src/video_core/query_cache/query_cache_base.h:39`, `:56`; `src/video_core/renderer_vulkan/vk_query_cache.h:29`, traits `:72` | Traits::RuntimeType. GetStreamerInterface returns a StreamerInterface for each QueryType; SyncValues writes guest values; Bind3DEngine/View3DRegs, Barriers, End/Pause/ResumeHostConditionalRendering and compare-one/compare-two conditional operations. Streamer ownership, bank recycling and CPU guest writes must survive invalidation and channel changes. |
| Query streamers | `src/video_core/query_cache/query_cache.h:48`, `:119`, `:179` | GuestStreamer/StubStreamer and QueryCacheBase implementation expose actual runtime calls. Follow `query_stream.h`/`bank_base.h` for host streamer/bank contracts; this checkout is not the older HostCounter-only query cache. |
| `FenceManager<Traits>` | `src/video_core/fence_manager.h:42`, protected hooks `:154`; Vulkan traits `src/video_core/renderer_vulkan/vk_fence_manager.h:43` | FenceType and three cache types; CreateFence, QueueFence, IsFenceSignaled, WaitFence and HAS_ASYNC_CHECK. Flush cache batches before signalling; release guest callbacks/syncpoints only after the represented work completes. |
| Rasterizer integration | `src/video_core/rasterizer_interface.h` and `src/video_core/renderer_vulkan/vk_rasterizer.h` | Bind channels/engines, invoke caches, handle CPU writes/unmaps, draw/dispatch/clear, barriers, acceleration and disk-resource loading. P1 implements the complete virtual interface with explicit stubs instead of instantiating incomplete templates. |

Use `texture_cache/texture_cache.h`, `buffer_cache/buffer_cache.h` and
`query_cache/query_cache.h` as the final authority for instantiated calls. Vulkan
helpers such as MSAA resolve shadows, VkFormat lists and render-pass caching are
backend implementation details, not requirements to reproduce by name.

## SPIR-V to DXIL integration

Use Mesa commit `15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`, the same pin as the
existing UWP Gallium build. Its [Meson target](https://github.com/aerisarn/mesa-uwp/blob/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/src/microsoft/spirv_to_dxil/meson.build)
provides shared/static `spirv_to_dxil` and the `spirv2dxil` executable when
`-Dspirv-to-dxil=true`. `build-mesa.cmd` now enables it alongside Gallium WGL.
The workflow stages the header and `dxil_versions.h`, import/static libraries,
compiler DLL/CLI and the existing `prepare_dxil.py` validator/notices into
`nxbox-spirv-to-dxil`; the ordinary Mesa artifact also retains installed files.
P1 does not link or load that DLL. P3 must add native package staging and verify its
UWP imports, independently of the host CLI.

The pinned [C API](https://github.com/aerisarn/mesa-uwp/blob/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/src/microsoft/spirv_to_dxil/spirv_to_dxil.h)
accepts words, stage/entrypoint, specialization values, validator-version limit,
runtime configuration and a logger. It returns owned DXIL plus runtime-data metadata;
copy the result into the pipeline cache, then call `spirv_to_dxil_free`. Load the
packaged validator before validation; preserve diagnostics and fail the pipeline
cleanly instead of passing invalid bytes to PSO creation. Limit shader model and
validator version to supported values, then validate and retain the signed container.

The following is the planned adapter, not an implemented shader path:

1. Reuse Maxwell → IR translation and `EmitSPIRV` from
   `src/shader_recompiler/backend/spirv/emit_spirv.h:36`. Mirror stage chaining,
   dual VA/VB merging and runtime info from `vk_pipeline_cache.cpp:841`. Construct
   a D3D12-specific `Shader::Profile` from actual capabilities and Mesa lowering
   support; do not copy all Vulkan feature flags. Unsupported SPIR-V capabilities
   need explicit IR lowering or a reported failure.
2. Build one binding schema from Shader::Info and the shared `Shader::Backend::Bindings`
   counter. Eden emits set 0 decorations (`spirv_emit_context.cpp:288`). Remap
   DescriptorSet/Binding deterministically to D3D12 register spaces/registers,
   using nonoverlapping ranges for descriptor arrays. Combined sampled images
   need matching SRV and sampler entries; writable storage images/buffers need
   UAVs, uniform buffers CBVs. Root tables, shader decorations and heap population
   must consume the same schema. Reserve a separate register space for runtime
   data and push constants. Keep read-only-image inference disabled initially so
   Mesa cannot silently disagree with the descriptor type chosen by the host.
3. Mesa's push-constant configuration names a CBV register/space. Bind a root
   constant block at that same register/space with the emitted byte offsets and
   DWORD count (SetGraphicsRoot32BitConstants / SetComputeRoot32BitConstants).
   Account for tables in the 64-DWORD root budget. Oversized blocks require an
   aligned upload CBV fallback. Prove constant offsets with a shader test before
   compiling guest pipelines; root constants are a host binding choice, not a
   separate Mesa push-constant option.
4. Pass real SpecId/value pairs through `dxil_spirv_specialization`. Much of Eden's
   specialization already happens in Profile/RuntimeInfo/IR before SPIR-V emission;
   do not invent Vulkan specialization constants absent from the module. Include
   these inputs, binding/root schema, shader hashes, Mesa compiler version, shader
   model/validator limits and fixed PSO state in cache keys. Never reuse GL/Vulkan
   disk blobs as DXIL.
5. Supply the runtime-data CBV when returned metadata requires it (vertex/base
   instance/draw ID/viewport correction or compute group/base values). Check
   coordinate/depth conventions once, at this boundary, including viewport flips,
   tessellation, geometry, clip/cull distance and texture-buffer emulation.

## Phases and acceptance criteria

| Phase | Deliverable | Acceptance criteria |
| --- | --- | --- |
| P1: skeleton, clear, present | Opt-in RendererD3D12 and complete stub rasterizer; CoreWindow swapchain, direct submission/fence; architecture and shader artifacts | Existing host suite passes with its real exit code; OpenGL remains selected without opt-in; Windows UWP build/link succeeds; console displays the documented color, frame notifications advance, suspend/resume and shutdown do not hang. No Mesa load or duplicate CoreWindow swapchain in native mode. Current host work does not establish the Windows/console criteria. |
| P2: buffers and textures | Native TextureCache/BufferCache traits, resources, staging, state tracker, allocation/budget policy and framebuffer presentation | Instantiate shared templates; round-trip linear and block-linear data, mips/layers, BC and CPU ASTC, null views, format aliases, CPU invalidation and fence-retired reuse. Present a known guest framebuffer with crop/transform/scaling and compare readback. No shader-based guest rendering required yet. |
| P3: graphics pipelines | SPIR-V adapter, validated DXIL, root signatures, descriptor heaps and graphics PSO cache | Known shaders demonstrate constants, descriptor arrays, vertex inputs and sampled/storage textures; triangle and guest scene draw correctly without GL. Validate all stage combinations used; rejected shaders/PSOs retain diagnostics. Warm disk cache reconstructs identical output; no stale descriptor reuse. |
| P4: compute | Native compute PSOs, runtime data, Dispatch and UAV synchronization | Deterministic SSBO/image workloads match CPU/Vulkan references; guest compute-to-graphics dependencies and barriers work. No compute silently dropped; unsupported features are reported. |
| P5: queries and transform feedback | Query heaps/readback, guest reports, predication and stream output | Occlusion/timestamp/report ordering and conditional draws match reference; stream-output bytes and counters match; guest waits complete correctly and no query/bank is reused before fence retirement. Test software fallbacks for unavailable statistics. |
| P6: parity | Presentation layers, remaining formats/state/features, cache robustness, diagnostics and performance | Compare representative games/scenes against the existing backend; screenshots and saves/session behavior remain correct; exercise resize/suspend/resume/device loss, long-run memory and cold/warm shader cache. No silent unsupported paths; establish budgets/performance before any default-backend change. |

## Validation commands and outstanding evidence

```sh
cd tests/port && python3 -m unittest discover -s . -p 'test_*.py'
```

The selection test compiles std-only C++ on the host and checks precedence,
case handling and persistence of backend enum values. Full Windows compilation
requires the existing Visual Studio UWP toolchain (`tools/nxbox/build-uwp.cmd`).
The Mesa workflow builds the new shader artifacts; no artifact is generated locally
on macOS by this change. For console evidence, keep startup logs, the test-color
capture, frame progression and teardown/removal logs. Do not treat passing host
checks as proof that the Xbox accepts the swapchain or shader compiler imports.
