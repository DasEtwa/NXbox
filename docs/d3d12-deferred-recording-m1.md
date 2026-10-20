# Deferred D3D12 command recording: M1 audit

Scope: source audit only on `perf/deferred-recording`, based on `perf/1.0.6`.
No renderer change, recorder, environment switch, or performance result is delivered in M1.
Paths below are relative to `src/video_core/renderer_d3d12/` unless stated otherwise.

## Findings and justified refinements to the requested design

- `Context::Commands()` returns `ID3D12GraphicsCommandList*` (`d3d12_context.h:476`). A subset facade cannot be substituted for that COM interface by a cast. Use a typed forwarding facade for both paths (immediate forwards to the existing real list; deferred encodes), with the immediate path preserving the same D3D12 calls and order. Change explicit pointer signatures in `d3d12_descriptor.h:200`, `d3d12_descriptor.cpp:504`, and the draw functor in `d3d12_rasterizer.cpp:147`. Most `auto*` call sites can remain unchanged. This is a necessary type-level refinement; literal byte-identical executable code is not achievable with a changed return type. The disabled path must preserve behavior and stream-free execution.
- `QueryInterface` at `d3d12_rasterizer.cpp:1983,1997` returns list8/list1 for independent stencil/depth bounds. Do not return the real list from the facade: that would bypass FIFO order. Add facade methods for `OMSetFrontAndBackStencilRef` and `OMSetDepthBounds`; acquire/check real extended interfaces on recorder initialization and preserve the existing unsupported-interface error when the feature is requested. An alternative full COM proxy would require the entire interface hierarchy, rather than the requested subset, with substantially greater audit scope.
- Direct member calls (`d3d12_context.cpp:1078,1104,1129,1130,1228,1281,1304,1309,1361,1370`) also need routing through the facade. Replacing only `Commands()` leaves timestamps, markers, and most transitions on the producer, causing races and incorrect ordering.
- `CurrentTick()` is `LastSubmittedTick()+1` (`d3d12_context.h:521`), which is only valid for synchronous submission. Introduce separate producer next/current, sealed/enqueued, recorder acknowledged/signalled, and completed fence values. Empty `Submit()` must return the latest sealed tick even if recorder acknowledgement is pending (`d3d12_context.cpp:1123`).
- `Present` currently immediately follows `Submit` (`renderer_d3d12.cpp:368-381`). Keep DXGI and window work on the producer, but wait for recorder acknowledgement of that submission before calling `Present`; waiting for GPU completion is unnecessary here. Otherwise DXGI may enqueue presentation before the render commands have reached the queue.

## Complete command-list method inventory

Includes `Commands()->`, local `commands`/`list` aliases, direct Context member calls, and list1/list8 methods. The only real list creation and queue execution found under `src` are in Context. Inventory is a source enumeration, not proof that every path executes in a given game. Lifecycle/object/interface operations are distinguished below from recordable commands.

| Method | Call sites |
| --- | --- |
| `BeginQuery` | `d3d12_query_cache.cpp:557`, `d3d12_query_cache.cpp:559`, `d3d12_query_cache.cpp:563` |
| `ClearDepthStencilView` | `d3d12_clear.cpp:211` |
| `ClearRenderTargetView` | `d3d12_clear.cpp:188`, `renderer_d3d12.cpp:248` |
| `Close` | `d3d12_context.cpp:887`, `d3d12_context.cpp:1141` |
| `CopyBufferRegion` | `d3d12_buffer_cache.cpp:107`, `d3d12_buffer_cache.cpp:245`, `d3d12_compute_pass.cpp:85`, `d3d12_query_cache.cpp:278`, `d3d12_query_cache.cpp:447`, `d3d12_query_cache.cpp:656`, `d3d12_query_cache.cpp:718`, `d3d12_query_cache.cpp:851`, `d3d12_query_cache.cpp:871`, `d3d12_query_cache.cpp:901`, `d3d12_rasterizer.cpp:1345`, `d3d12_rasterizer.cpp:1397`, `d3d12_rasterizer.cpp:1402`, `d3d12_rasterizer.cpp:1408`, `d3d12_rasterizer.cpp:2041`, `d3d12_rasterizer.cpp:2070`, `d3d12_rasterizer.cpp:2284`, `d3d12_rasterizer.cpp:2379`, `d3d12_texture_cache.cpp:428`, `d3d12_texture_cache.cpp:582`, `d3d12_texture_cache.cpp:587`, `d3d12_transform_feedback.cpp:27`, `d3d12_transform_feedback.cpp:124`, `d3d12_transform_feedback.cpp:188`, `d3d12_transform_feedback.cpp:207` |
| `CopyResource` | `d3d12_blit_image.cpp:306`, `d3d12_blit_image.cpp:404`, `d3d12_blit_image.cpp:411`, `d3d12_rasterizer.cpp:396` |
| `CopyTextureRegion` | `d3d12_blit_image.cpp:743`, `d3d12_rasterizer.cpp:408`, `d3d12_texture_cache.cpp:559`, `d3d12_texture_cache.cpp:571`, `d3d12_texture_cache.cpp:802`, `d3d12_texture_cache.cpp:810`, `d3d12_texture_cache.cpp:1405`, `d3d12_texture_cache.cpp:1413`, `d3d12_texture_cache.cpp:1643`, `renderer_d3d12.cpp:337` |
| `Dispatch` | `d3d12_dispatch.cpp:151`, `d3d12_query_cache.cpp:312` |
| `DrawIndexedInstanced` | `d3d12_rasterizer.cpp:151` |
| `DrawInstanced` | `d3d12_blit_image.cpp:542`, `d3d12_blit_image.cpp:552`, `d3d12_clear.cpp:166`, `d3d12_present/present.cpp:183`, `d3d12_rasterizer.cpp:155`, `d3d12_rasterizer.cpp:2903`, `renderer_d3d12.cpp:302` |
| `EndQuery` | `d3d12_context.cpp:1078`, `d3d12_context.cpp:1129`, `d3d12_query_cache.cpp:601`, `d3d12_query_cache.cpp:925` |
| `ExecuteIndirect` | `d3d12_dispatch.cpp:167` |
| `IASetIndexBuffer` | `d3d12_rasterizer.cpp:1973` |
| `IASetPrimitiveTopology` | `d3d12_blit_image.cpp:535`, `d3d12_clear.cpp:165`, `d3d12_present/present.cpp:182`, `d3d12_rasterizer.cpp:1892`, `d3d12_rasterizer.cpp:2897`, `renderer_d3d12.cpp:252` |
| `IASetVertexBuffers` | `d3d12_rasterizer.cpp:1971` |
| `OMSetBlendFactor` | `d3d12_rasterizer.cpp:1992` |
| `OMSetDepthBounds` | `d3d12_rasterizer.cpp:1999` |
| `OMSetFrontAndBackStencilRef` | `d3d12_rasterizer.cpp:1985` |
| `OMSetRenderTargets` | `d3d12_blit_image.cpp:537`, `d3d12_blit_image.cpp:539`, `d3d12_clear.cpp:156`, `d3d12_clear.cpp:159`, `d3d12_dispatch.cpp:115`, `d3d12_present/present.cpp:179`, `d3d12_query_cache.cpp:297`, `d3d12_rasterizer.cpp:1914`, `d3d12_rasterizer.cpp:2890`, `renderer_d3d12.cpp:246` |
| `OMSetStencilRef` | `d3d12_blit_image.cpp:550`, `d3d12_clear.cpp:157`, `d3d12_rasterizer.cpp:1987` |
| `QueryInterface` | `d3d12_rasterizer.cpp:1983`, `d3d12_rasterizer.cpp:1997` |
| `RSSetScissorRects` | `d3d12_blit_image.cpp:534`, `d3d12_clear.cpp:164`, `d3d12_present/present.cpp:181`, `d3d12_rasterizer.cpp:1969`, `d3d12_rasterizer.cpp:2896`, `renderer_d3d12.cpp:263` |
| `RSSetViewports` | `d3d12_blit_image.cpp:533`, `d3d12_clear.cpp:163`, `d3d12_present/present.cpp:180`, `d3d12_rasterizer.cpp:1967`, `d3d12_rasterizer.cpp:2895`, `renderer_d3d12.cpp:262` |
| `Reset` | `d3d12_context.cpp:1069` |
| `ResolveQueryData` | `d3d12_context.cpp:1130`, `d3d12_query_cache.cpp:602`, `d3d12_query_cache.cpp:927` |
| `ResolveSubresource` | `d3d12_blit_image.cpp:605` |
| `ResourceBarrier` | `d3d12_buffer_cache.cpp:238`, `d3d12_context.cpp:1228`, `d3d12_context.cpp:1281`, `d3d12_context.cpp:1304`, `d3d12_context.cpp:1309`, `d3d12_context.cpp:1361`, `d3d12_context.cpp:1370`, `d3d12_context.cpp:1385`, `d3d12_context.cpp:1456`, `d3d12_context.cpp:1670`, `d3d12_dispatch.cpp:58`, `d3d12_rasterizer.cpp:3556` |
| `SOSetTargets` | `d3d12_transform_feedback.cpp:152`, `d3d12_transform_feedback.cpp:161` |
| `SetComputeRoot32BitConstants` | `d3d12_dispatch.cpp:134`, `d3d12_query_cache.cpp:302` |
| `SetComputeRootConstantBufferView` | `d3d12_dispatch.cpp:140` |
| `SetComputeRootDescriptorTable` | `d3d12_dispatch.cpp:136` |
| `SetComputeRootShaderResourceView` | `d3d12_query_cache.cpp:303`, `d3d12_query_cache.cpp:305` |
| `SetComputeRootSignature` | `d3d12_dispatch.cpp:132`, `d3d12_query_cache.cpp:298` |
| `SetComputeRootUnorderedAccessView` | `d3d12_query_cache.cpp:308`, `d3d12_query_cache.cpp:310` |
| `SetDescriptorHeaps` | `d3d12_blit_image.cpp:529`, `d3d12_descriptor.cpp:517`, `d3d12_dispatch.cpp:131`, `d3d12_present/present.cpp:174`, `renderer_d3d12.cpp:251` |
| `SetGraphicsRoot32BitConstants` | `d3d12_blit_image.cpp:541`, `d3d12_blit_image.cpp:551`, `d3d12_clear.cpp:154`, `d3d12_present/present.cpp:178`, `d3d12_rasterizer.cpp:2003`, `d3d12_rasterizer.cpp:2888`, `renderer_d3d12.cpp:301` |
| `SetGraphicsRootConstantBufferView` | `d3d12_rasterizer.cpp:1906` |
| `SetGraphicsRootDescriptorTable` | `d3d12_blit_image.cpp:531`, `d3d12_present/present.cpp:177`, `d3d12_rasterizer.cpp:1895`, `d3d12_rasterizer.cpp:2869`, `d3d12_rasterizer.cpp:2870`, `d3d12_rasterizer.cpp:2871`, `d3d12_rasterizer.cpp:2872`, `renderer_d3d12.cpp:300` |
| `SetGraphicsRootSignature` | `d3d12_blit_image.cpp:530`, `d3d12_clear.cpp:148`, `d3d12_present/present.cpp:175`, `d3d12_rasterizer.cpp:1885`, `d3d12_rasterizer.cpp:2867`, `renderer_d3d12.cpp:249` |
| `SetMarker` | `d3d12_context.cpp:1104` |
| `SetName` | `d3d12_context.cpp:1152` |
| `SetPipelineState` | `d3d12_blit_image.cpp:540`, `d3d12_blit_image.cpp:547`, `d3d12_clear.cpp:147`, `d3d12_dispatch.cpp:133`, `d3d12_present/present.cpp:176`, `d3d12_query_cache.cpp:299`, `d3d12_rasterizer.cpp:1889`, `d3d12_rasterizer.cpp:2868`, `renderer_d3d12.cpp:295` |
| `SetPredication` | `d3d12_query_cache.cpp:273`, `d3d12_query_cache.cpp:285`, `d3d12_query_cache.cpp:729`, `d3d12_query_cache.cpp:743`, `d3d12_transform_feedback.cpp:16`, `d3d12_transform_feedback.cpp:114`, `d3d12_transform_feedback.cpp:201` |

### Encoding contract

Recordable kinds are every inventory entry except `Close`, `Reset`, `SetName`, and `QueryInterface`. `Close`/`Reset` belong to recorder lifecycle records; `SetName` is submission metadata consumed by the recorder; `QueryInterface` is replaced by typed extended methods as described above. `ExecuteCommandLists` and `Signal` are submission-control operations, not graphics-list methods.

- Copy scalar arguments verbatim, including signed base vertices, 64-bit offsets/addresses and descriptor handles. Preserve nulls and optional arguments distinctly from zero-filled structures.
- Copy arrays for barriers, viewports, scissors, vertex/stream-output views, heap pointers, root constants (DWORD count), rectangles, blend/color floats, and marker bytes. Copy optional index-buffer views, copy locations, source boxes, depth handles and clear rectangles into the stream. Never store a pointer to caller stack/scratch/vector storage.
- `OMSetRenderTargets` with contiguous handles (`TRUE`) copies one base handle, not `NumRenderTargets` caller elements; the noncontiguous case copies that many handles. Zero targets and null depth are valid. Preserve this distinction in fake replay tests.
- For barriers inspect the active union: transition resource, both alias resources (either can be null), UAV resource (can be null for global barrier). CopyTextureRegion must copy both location unions and retain their resource pointers. ExecuteIndirect must hold signature, argument buffer and optional count buffer. Queries hold heap and resolve destination. SetPredication permits null resource.
- GPU virtual addresses/descriptor handles have no COM identity to AddRef. Existing owning Resource leases and descriptor-slot fence leases remain mandatory. Holding a heap does not prevent its slots from being overwritten.

## Tick, fence and wait chain

Current path: producer Commands opens/reset allocator -> CurrentTick tags staging/descriptors/query banks -> Submit closes/executes -> moves callbacks/resources into Batch(tick) -> queue Signal(tick) -> publishes submitted -> completion thread WaitSubmitted -> fence event -> CompleteThrough executes callbacks and releases batch resources (`d3d12_context.cpp:1029-1188`; `d3d12_fence_manager.cpp:10-30`).

Deferred path required:

1. Producer opens a logical recording tagged T, invalidates recorded state and updates resource-state/copy-hazard tracking on that thread only. Every allocation and lease referring to that recording uses T; recorder lag must never change its tag.
2. Submit seals FIFO work through T, associates resources/callbacks and diagnostics with T before publishing it, advances producer tick, and returns T. Do not publish T as actually submitted yet. Queue empty submits retain current no-op semantics. Publish partial chunks at Submit even when they are not full.
3. Recorder opens/resets the real allocator/list, replays FIFO commands, emits matching timestamp boundaries, closes, checks device removal, ExecuteCommandLists, checks removal, Signal(fence,T), then release-publishes acknowledged T and wakes submission waiters. Sixteen allocator slots remain bounded (`d3d12_context.h:581`); only recorder chooses/resets them, after that slot's real fence completes.
4. Wait(T) may seal the open logical T on the producer. It then publishes/wakes the stream and awaits acknowledgement >= T (or failure) before the GPU fence wait. Already sealed ticks must not be mistaken for invalid future ticks merely because acknowledgement lags. Preserve rejection of truly future ticks.
5. WaitSubmitted(T) cannot seal producer state: it is used on the completion thread. All enqueued submits must be published/wakeable without requiring the producer to make another call. It waits for recorder acknowledgement/failure first, then uses existing spin/event/device-loss checks and completes callbacks. Never wait holding the producer chunk, completion mutex, or generic fence guard that the recorder needs to progress.
6. IsFree(T) remains a nonblocking poll: false unless acknowledgement >= T and actual completed fence >= T; retain UINT64_MAX/device-loss checks. Do NOT seal or wait for the currently open T in IsFree: descriptor/staging allocation calls it on open work (`d3d12_descriptor.cpp:483,526`; `d3d12_context.cpp:1688`). A blocking interpretation would destroy batching or deadlock. This is a deliberate refinement of the spec's flush/wait wording for a polling API; blocking waits enforce the full handshake.
7. Collect snapshots completed fence only after validating device status and clamps retirement to acknowledged/completed work. CompleteThrough must never run callbacks based solely on an enqueued tick; require acknowledged and completed coverage. It remains on existing producer/completion callers, NOT the recorder: callbacks can reenter Commands/Wait and would otherwise introduce a second stream producer or make the recorder wait on itself. Keep existing ordered, detached batch retirement (`d3d12_context.cpp:1001`).
8. IsTickSubmitted must mean acknowledged, with the failure escape retained. Generic fence thread waits on that predicate (`src/video_core/fence_manager.h:269`); notify it after real submission (or preserve its timed backstop), not just when GPU thread enqueues work. With fence batching disabled `IsFenceSubmitted` currently always returns true (`d3d12_fence_manager.cpp:107`), so WaitSubmitted's acknowledgement gate remains essential in that configuration too.
9. Recorder failure stores an exception/status before wake-all, marks submit_failed, stops new work, and makes all waiters report failure rather than await an unsignallable tick. Preserve checks around execution, fence UINT64_MAX, event timeout and DRED reporting. Shutdown seals producer work, waits/drains where possible, stops/joins recorder before releasing queues/lists/fences or callback owners; failure shutdown must also wake blocked producer chunk acquisition.

### Every completion-dependent caller found

- Finish: lifecycle/memory pressure/allocation retry/staging pressure (`d3d12_context.cpp:889,1194,1656,1677,1748`). Resource creation can reenter Finish before the list is opened; preserve the existing reentrancy checks.
- Allocator reuse (`d3d12_context.cpp:1046`): move real fence-only allocator wait onto recorder; it cannot use an acknowledgement helper that waits on itself.
- Descriptor rings (`d3d12_descriptor.cpp:472-546`): tag with logical current T, poll safely, seal current T on exhaustion, wait on oldest blocking T.
- Staging pages (`d3d12_context.cpp:1683-1772`): tag each appended use with logical T; recycle only after real fence completion AND sole page ownership. Keep upload/readback data alive and immutable through GPU completion, not just replay.
- Buffer write/reuse bridge (`d3d12_buffer_cache.h:62-69`), guest-visible CPU readbacks and snapshots: all Context::Wait calls inherit acknowledgement-first semantics. Copy hazard checks (`d3d12_context.cpp:1291,1332`) must use logical ticks while resource-state tracking stays producer-owned.
- Query bank retirement (`d3d12_query_cache.cpp:362-367`), timestamp readback (`:921-940`), query teardown (`:495`): same signal T; no CPU read before completion.
- Guest fences/syncpoints (`d3d12_fence_manager.cpp:10-30,95-108`): don't signal guest completion at stream publication or recorder replay; only GPU fence completion.
- Backbuffer reuse (`renderer_d3d12.cpp:242`), screenshot drain, resize (`:355`), presenter BeginFrame/destruction (`d3d12_present/present.cpp:202-212`), title pipeline cache replacement (`d3d12_pipeline_cache.cpp:1980`): drain acknowledged AND GPU-completed work before descriptor overwrite or object destruction.

## Lifetime audit

| Object class / evidence | Current ownership and deferred requirement |
| --- | --- |
| Resource wrappers and placed heaps (`d3d12_context.cpp:1198-1228,1665-1673`) | Retain/Transition add ResourcePtr to the batch with a per-recording generation stamp. Keep the ResourcePtr, not only ID3D12Resource: wrapper destruction can return placed slots to BufferPool. Preserve holds through real GPU fence T; COM retention in stream only covers replay and cannot replace these leases. |
| Temporary copy/readback/scratch resources (`d3d12_texture_cache.cpp`, `d3d12_rasterizer.cpp`, `d3d12_blit_image.cpp:304,402`) | Existing Transition/Retain plus batch callbacks cover many uses. Each encoded resource pointer still needs an owned hold until replay; inspect resources embedded in copy/barrier unions. Raw GPU-address binds require original ResourcePtr batch retention. Do not release all stream holds before batch ownership is secured. |
| Graphics PSOs/root signatures (`d3d12_rasterizer.cpp:1885-1889`; `d3d12_graphics_pipeline.h:117-118`) | Owned by pipeline caches, not Retain. Title map replacement calls Finish (`d3d12_pipeline_cache.cpp:1980`). Explicit COM leases on bind commands protect replay even across exceptions; keep through completion unless an audited owner/drain contract already guarantees GPU lifetime. |
| Compute PSO/root/heaps/indirect signature (`d3d12_dispatch.cpp:112-114,167`) | Existing AfterCompletion captures cover these objects; maintain them in the sealed T batch. Encoder holds pointer arguments, including indirect signature/count buffers, through replay. |
| Clear PSO/root (`d3d12_clear.cpp:168`) | Existing AfterCompletion captures; keep T association. Because this hold is registered after drawing, encoder retention must protect partially encoded work if a later operation throws. |
| Blit heaps/PSOs/root (`d3d12_blit_image.cpp:224-228,522`) | Descriptors callback captures a shared object whose SharedState owns root/PSO map. Existing hold is registered before recording. Preserve it through fence T, plus stream holds for referenced COM objects. |
| Main descriptor allocator (`d3d12_descriptor.cpp:457-546`) | Heap members are long-lived; slots are tagged CurrentTick and retired by IsFree. Heap AddRef alone is insufficient. Correct logical ticks and acknowledgement-aware completion retain slot contents through replay AND GPU reads. CPU RTV/DSV descriptors must survive until the delayed list method consumes them. |
| Query heaps and resolve root/PSO (`d3d12_query_cache.cpp:465-469,495,601`) | Members owned by query runtime and drained on destruction; acquire encoded COM holds for Begin/End/Resolve and resolve PSO/root. Query bank indices are protected by bank fence T, not merely heap lifetime. |
| Temporary timestamp heap (`d3d12_query_cache.cpp:924-929`) | Existing callback captures heap and Retain holds readback. Preserve both, copy query scalars. |
| Profile query heap/readback (`d3d12_context.cpp:1049-1064,1127-1139`) | Context-owned; recorder controls timestamp slot indices and must capture per-submit index before producer advances. Readback callback only after real T completion; avoid sharing mutable profiler maps/counters across threads. |
| Swapchain buffers/present heaps/PSOs/root (`renderer_d3d12.cpp:60-78,242-302`) | Context retains buffer resources; Impl drains before destruction. Present descriptor ranges wait for previous buffer tick. Submission acknowledgement before DXGI Present plus completion before resize/reuse are both required. |
| Alternate Presenter (`d3d12_present/present.cpp:38-42,160-212`) | Member COM owners and scratch resources; BeginFrame Finish protects cursor reset/descriptor reuse, destructor drains. Preserve these stronger drains in first implementation. |

The audit establishes ownership mechanisms and required holds, not a claim that a future encoder is lifetime-safe. M3 must explicitly destroy caller temporaries immediately after encoding and test wrapper/placed-slot leases separately from COM reference counts.

## Work staying on the GPU/producer thread

Device Create*, CopyDescriptors*, GetDesc/GetGPUVirtualAddress, Map/Unmap, resource/upload preparation, descriptor writes, pipeline lookup/creation and state-cache comparisons remain synchronous. They return results or mutate CPU-visible resources needed for subsequent emulation. Preserve descriptor contents until replay; merely copying the descriptor handle into the stream cannot snapshot the descriptor. No queue Wait calls were found in this renderer; any future queue wait must preserve order with recorder submits rather than issue ahead of them. GetTimestampFrequency stays synchronous. Window/swapchain initialization, current-buffer lookup, Present and ResizeBuffers remain producer operations with the ordering/drain boundaries above. Real allocator/list creation/reset/close, list naming, list interface acquisition, ExecuteCommandLists and Signal belong to recorder control in deferred mode. Device removal diagnostics and info-queue access need coordinated ownership to avoid racing ClearStoredMessages.

## Bounded SPSC plan and host proof required for M2/M3

Use a fixed preallocated pool of aligned chunks and two SPSC ownership queues (ready and recycled); per-call POD headers/payload copy and optional AddRef write into exclusively producer-owned storage without heap allocation or mutex. Publish with release, consume with acquire; consumer returns chunk only after replay and owned-hold transfer/release. Copy large variable arrays across bounded records/chunks with defined alignment/length validation; no implicit vector fallback. At exhaustion publish partial work and use boundary backpressure, with failure/shutdown wakeup. Specify whether the no-lock constraint permits a sleeping boundary wait; never add a lock per command. Recorder allocator waits must not invoke producer callbacks. Existing Retain vectors/callback allocations are baseline behavior, not a justification for new stream allocations per draw.

Fake-list host tests must cover every recordable method listed above plus begin/submit lifecycle, exact sequence/args, zero-length/null/contiguous-handle cases, extended interfaces, temporary inputs/objects destroyed after enqueue, exceptions, >16 submits, partial-chunk flush, pool exhaustion, failure wakeup, producer/consumer stress and recycling without overwrite. Test delayed acknowledgement separately from GPU completion and open-vs-sealed ticks. Run independent clang++ C++20 ASan+UBSan and TSan binaries; no Windows headers in pure stream logic. Port tests must have no new failures. M1 makes no claim of passing these not-yet-written recorder tests.

## Performance hypothesis and tester protocol

Supplied console measurements give a gross phase budget, not an achievable saving: warm MK8D 3.39+0.98+1.00 = 5.37 ms/frame; BotW 18.3+3.25+3.5 = 25.05 ms/frame. Those scopes include CPU work staying on the producer. Encoding, retention, backpressure and Present acknowledgement cost time; the recorder may become a bottleneck. No measured speedup or FPS projection is supported by M1.

For the current package, best next experiment: preserve all other settings in `LocalState\nxbox_env.txt`, set `NXBOX_D3D12_FRAME_PROFILE=1` and `NXBOX_D3D12_DEFERRED_RECORDING=0`, fully restart, then capture a repeatable warm scene in MK8D, BotW and Wonder. Save `eden_uwp_diag.txt` with package identity and scene details. The deferred variable has no implementation in M1, so setting it to 1 now is NOT an A/B test.

After M2/M3 and a lead-built package exist, repeat the identical scenes with switch 0 vs 1, restart each run (read-once switch), use the same warmup and measurement duration, and repeat 0 after 1 to check variance. Require one device-creation log naming the selected mode. Record `draw_recording`, `barriers`, `submit`, `fence_wait`, `sync_wait` ms/frame, fps, GPU-thread CPU%, recorder CPU%, GPU busy time, submit counts and waits/backpressure. Keep submit trace off for timing because it waits on every submit. Success is lower GPU-thread time with better fps and no offsetting wait increase, identical screenshots/motion/query-dependent behavior, no device loss, no guest-fence hangs, correct screenshots/resize/title switching and stable bounded memory. Wonder is especially sensitive to existing sync waits. Console, CI and package run IDs are unavailable: this task forbids network/console use.

## M1 validation

Before documentation changes: `python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'` completed with `Ran 218 tests in 69.992s` and `OK`; no pre-existing failures. Captured output: `/tmp/p3-rec-baseline.log`. No new code was written, so recorder sanitizer tests are not applicable yet. `git diff --check` passed. No CI, package, network, console, commit or push operations were performed.
