# Native D3D12 backend

## Scope and selection

NXbox currently uses Eden OpenGL (SPIR-V shaders by default), Mesa Gallium d3d12,
then D3D12. The native backend removes the OpenGL state machine and Gallium driver
from this path. It retains Eden's Maxwell decoding, IR and VideoCommon cache algorithms.
The native shader path is Maxwell IR → HLSL → DXC → DXIL. A new textual HLSL
backend will live next to Eden's GLSL backend in `src/shader_recompiler/backend/`.
Neither Mesa nor `spirv_to_dxil` participates in this path. Vulkan is the architectural reference, not a runtime
dependency of the native renderer. Existing Vulkan sources remain part of Eden's build.

The native backend lives in `src/video_core/renderer_d3d12/`. It is compiled only when `WIN32` and
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
returns a no-op `GraphicsContext` and does not load Mesa or WGL. Native presentation loads the packaged `dxcompiler.dll`
and uses DXC validation to produce DXIL. The library
and launch screen release their Direct2D swapchain before handing over CoreWindow.
Native launch-frame uploads remain a frontend TODO; they must use the renderer's
swapchain.

P1 creates a hardware D3D12 device at feature level 11_0, direct queue, event-backed
fence, allocator/list, two RTVs and an opaque RGBA8 flip-discard CoreWindow swapchain.
P2 retains serialized submission: allocator and presentation descriptors are reused
only after the previous fence completes. `Context` is shared by the renderer and
both cache runtimes; every command retains its resources until completion. Layout
changes retire work before releasing buffers and calling ResizeBuffers. Fence waits
are bounded at 10 seconds and check device removal. Presentation failure latches
presentation off and publishes `NXBOX_D3D12_COMMAND_FAILURE` for frontend shutdown.
Teardown drains submitted work, including partial construction.

### P2 implementation

- `d3d12_buffer_cache.*` and `d3d12_texture_cache.*` supply native parameter traits
  and explicitly instantiate `VideoCommon::BufferCache` / `TextureCache` in separate
  translation units. Channels, CPU invalidation, unmaps, downloads and frame ticks
  are connected to the rasterizer. Guest callbacks/syncpoints drain cache work and
  the queue before completing; the first implementation uses synchronous waits.
- Buffers use committed DEFAULT resources. UPLOAD and READBACK rings allocate
  512-byte-aligned slices in 4 MiB pages, retaining allocations by fence tick and
  shared leases. A page resets only after completion and after every lease retires.
  Each ring is capped at 64 MiB; all committed cache allocations share a hard
  512 MiB ceiling, including staging. Large buffer fills submit in 1 MiB chunks.
  Exhaustion reports an error rather than recycling live storage.
- Textures are committed DEFAULT resources with typeless storage for legal typed
  view casts. The std-only format table covers every Maxwell PixelFormat and marks
  missing mappings explicitly unsupported. CPU ASTC conversion uses VideoCommon,
  including the configured uncompressed/BC1/BC3 result; BC formats stay compressed.
  BC resource dimensions round up to whole blocks; partial BC regions must be
  block aligned. Texture transfers use `GetCopyableFootprints`, 256-byte row pitch
  and 512-byte placement alignment. Tight guest rows are repacked for `CopyTextureRegion`; readback
  unpacks only after fence completion. Mips, layers and 3D slices are addressed
  explicitly. A conservative whole-resource tracker emits transition barriers.
- `Composite` obtains a framebuffer view from the shared texture cache when one
  exists. Otherwise the texture runtime caches native framebuffer resources by
  guest address (bounded to 64 entries, with idle entries reclaimed) and uploads NVFlinger's block-linear guest memory, preserving the guest stride. This
  matches the existing presentation fallback's block-height assumption (log2=4).
  A built-in full-screen triangle, compiled with DXC at startup as `vs_6_0`/`ps_6_0`,
  handles crop, flips, rotation, scaling into the display rectangle and layer blend
  modes. Shader model 6.0 is checked separately from feature level. Each layer gets
  its own fence-retired SRV slot (up to 64). Empty frames retain the P1 test clear.

Guest draws, clears, compute, guest descriptors/samplers and pipelines remain stubs.
DMA/surface acceleration declines requests so engine CPU fallbacks run; inline
writes and payload/timestamp queries retain CPU behavior. Applet captures and
screenshots still return blank buffers. Unsupported packed/ETC formats, multi-plane
depth/stencil transfers, MSAA, incompatible reinterpretation, guest shader blits and
GPU unswizzle report unsupported operations rather than issuing invalid copies.
Converted ASTC has no reverse readback. Full parity for those paths is later work.

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
| Present layers/window adaptation | P2 uploads or looks up guest block-linear framebuffers and composes crop, transforms, scaling and layers with a built-in DXC shader. Guest graphics pipelines remain stubbed. |
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

## Maxwell IR to HLSL to DXIL integration (owner decision)

The native path is **Maxwell → Eden IR → textual HLSL → DXC → DXIL**. Implement a
new HLSL emitter next to `src/shader_recompiler/backend/glsl`, using the GLSL
backend as the textual-emission reference. Vulkan remains the reference for stage
chaining, cache ownership and runtime specialization. Do not emit SPIR-V, call Mesa,
load `spirv_to_dxil`, or add those dependencies to the native renderer. Existing
Mesa artifacts belong only to the unchanged OpenGL path.

`dxcompiler.dll` is already packaged with the app. P2 loads it with
`LoadPackagedLibrary` and `DxcCreateInstance` for the built-in presentation shader.
It uses the public `IDxcCompiler3` ABI, checks both invocation and compilation status,
retains diagnostics, and obtains the validated DXIL container before PSO creation.
The minimal local ABI declarations follow Microsoft's
[`dxcapi.h`](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/dxcapi.h).
No shader compilation import library or Mesa compiler library is linked.

P3 must extend this to guest shaders:

1. Reuse Maxwell decoding/IR, stage chaining and dual VA/VB merging. Emit HLSL from
   the same IR and Shader::Info, with a D3D12-specific Profile/RuntimeInfo derived
   from actual device capabilities. Lower unsupported IR operations explicitly or
   reject the shader with diagnostics. P2 does not implement the guest HLSL emitter.
2. Derive one binding schema from Shader::Info and the shared backend binding
   counters. HLSL `register`/`space` declarations, root signatures and descriptor
   population must use that schema: CBVs for uniform buffers, SRVs/samplers for
   sampled textures, UAVs for writable buffers/images. Descriptor arrays need
   nonoverlapping register ranges; reserve a space for runtime constants.
3. Match HLSL cbuffer layout and host constant offsets with shader tests. Keep root
   constants and tables within the 64-DWORD root budget; larger blocks use aligned
   upload CBVs. Handle base vertex/instance, draw ID and viewport/compute corrections
   in explicitly defined runtime data, without Mesa metadata.
4. Compile stage-appropriate HLSL profiles with the packaged DXC, targeting supported
   shader model/validator versions. Preserve errors, verify signed DXIL before PSO
   creation, and fail rejected guest pipelines cleanly. Test packaged UWP imports
   on Windows/Xbox; host tests cannot establish runtime DLL compatibility.
5. Include IR/shader hashes, Profile/RuntimeInfo specialization, emitter and DXC
   versions, register/root schema and fixed PSO state in native cache keys. Never
   reuse GL/Vulkan disk shader blobs. Verify coordinate/depth conventions, texture
   buffer emulation and all stage combinations at this boundary.

## Phases and acceptance criteria

| Phase | Deliverable | Acceptance criteria |
| --- | --- | --- |
| P1: skeleton, clear, present | Opt-in RendererD3D12 and complete stub rasterizer; CoreWindow swapchain, direct submission/fence; architecture | Existing host suite passes with its real exit code; OpenGL remains selected without opt-in; Windows UWP build/link succeeds; console displays the documented color, frame notifications advance, suspend/resume and shutdown do not hang. No Mesa load or duplicate CoreWindow swapchain in native mode. Current host work does not establish the Windows/console criteria. |
| P2: buffers and textures | Native TextureCache/BufferCache traits, committed resources, staging rings, state tracker, allocation budget and DXC framebuffer compositor | Instantiate shared templates; round-trip linear and block-linear data, mips/layers, BC and CPU ASTC, null views, format aliases, CPU invalidation and fence-retired reuse. Present a known guest framebuffer with crop/transform/scaling and compare readback. No shader-based guest rendering required yet. |
| P3: graphics pipelines | Maxwell IR HLSL emitter, DXC compilation, validated DXIL, root signatures, descriptor heaps and graphics PSO cache | Known shaders demonstrate constants, descriptor arrays, vertex inputs and sampled/storage textures; triangle and guest scene draw correctly without GL. Validate all stage combinations used; rejected shaders/PSOs retain diagnostics. Warm disk cache reconstructs identical output; no stale descriptor reuse. |
| P4: compute | Native compute PSOs, runtime data, Dispatch and UAV synchronization | Deterministic SSBO/image workloads match CPU/Vulkan references; guest compute-to-graphics dependencies and barriers work. No compute silently dropped; unsupported features are reported. |
| P5: queries and transform feedback | Query heaps/readback, guest reports, predication and stream output | Occlusion/timestamp/report ordering and conditional draws match reference; stream-output bytes and counters match; guest waits complete correctly and no query/bank is reused before fence retirement. Test software fallbacks for unavailable statistics. |
| P6: parity | Presentation layers, remaining formats/state/features, cache robustness, diagnostics and performance | Compare representative games/scenes against the existing backend; screenshots and saves/session behavior remain correct; exercise resize/suspend/resume/device loss, long-run memory and cold/warm shader cache. No silent unsupported paths; establish budgets/performance before any default-backend change. |

## Validation commands and outstanding evidence

```sh
cd tests/port && python3 -m unittest discover -s . -p 'test_*.py'
```

The selection test compiles std-only C++ on the host and checks precedence,
case handling and persistence of backend enum values. Resource tests execute the
format table, odd-sized BC footprints, mip/layer placement, overflow rejection and
row packing round trips under AddressSanitizer/UndefinedBehaviorSanitizer. Texture
layout rules follow Microsoft's [upload/readback documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data)
and [`GetCopyableFootprints`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints). Full Windows compilation
requires the existing Visual Studio UWP toolchain (`tools/nxbox/build-uwp.cmd`).
No guest HLSL shader artifacts are generated locally on macOS by this change.
Windows compilation, packaged DXC loading, GPU round trips and console framebuffer
comparisons remain required acceptance evidence; passing host tests alone does not
complete that evidence. For console evidence, keep startup logs, the test-color
capture, frame progression and teardown/removal logs. Do not treat passing host
checks as proof that the Xbox accepts the swapchain or shader compiler imports.
