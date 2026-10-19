# Deferred D3D12 recording: implementation, host proof and console protocol

M2/M3/M4 on `perf/deferred-recording`. Changes are uncommitted. The M1 inventory and
lifetime audit remain in `d3d12-deferred-recording-m1.md`.

## M2 implementation

`NXBOX_D3D12_DEFERRED_RECORDING` is read at device creation, defaults to 0, and is
logged once with `chunk_size=262144 chunk_count=8`. No stream, descriptor snapshot
heaps, or worker is created in mode 0. `CommandFacade` methods are forced inline;
the immediate path checks its latched stream pointer once and forwards the original
arguments to the original D3D12 method. Extended-interface calls retain their
original QueryInterface/call/release sequence. This is a source-level property,
not a measured claim of zero overhead on Xbox.

Mode 1 uses eight preallocated 256 KiB chunks, with one producer and one consumer.
Trivially copyable records contain a typed replay thunk, values, and offsets to
copied payloads. Records and arrays can cross chunk boundaries; payload starts
are aligned to 16 bytes. Producer encoding does not allocate. The consumer owns
a reusable aligned scratch vector, which can grow to the largest single record.
There is no mutex per recorded call. Atomic chunk ownership is synchronized only
at chunk boundaries. A full chunk is published immediately; a partial chunk is
published at Submit, explicit descriptor backpressure, and shutdown. Exhaustion
blocks at the next chunk boundary until the consumer recycles storage.

The recorder is named `D3D12 recorder` and uses `Common::ThreadPriority::Critical`,
the same priority as the GPU thread. It creates/resets/closes the real list and
allocators, replays commands, executes lists, checks removal before and after
ExecuteCommandLists, then signals the original fence value and acknowledges it.
It queries list1/list8 once. Unsupported extended methods are rejected on the
producer when requested, before encoding, with the existing error descriptions.
The direct member marker, timestamp and barrier calls also route through the
facade; lifecycle operations in the immediate-only Submit path remain native.

The producer's sealed tick, recorder acknowledgement (`submitted`), and GPU
completed fence remain distinct. Empty Submit returns the sealed tick. Open-tick
Wait seals and publishes before acknowledgement/GPU waits; WaitSubmitted never
flushes producer state. IsFree remains a poll, checking real completion and
acknowledgement. CompleteThrough clamps retirement to acknowledged work; its
callers supply completed ticks. Resource wrapper/placed-slot leases and callbacks
stay in fence-retired batches. The recorder's allocator wait does not execute
completion callbacks. Profiling timestamp slots additionally wait for their
completion callback before reuse. Present and swapchain operations stay on the
producer; Present waits for acknowledgement only. Resize and teardown still drain
the GPU. Teardown closes and joins the recorder before member destruction.

Replay holds cover COM pointers, pointer arrays, texture-copy locations and the
active resource-barrier union. The consumer drops these holds after replay,
including exceptions or abandoned commands. GPU-address and descriptor-slot leases
continue to use the original Resource wrappers and fence values. Existing cache
owners/drains still protect PSOs and roots through GPU completion; replay holds
are additional protection, not substitutes for GPU lifetime leases.

A necessary refinement to M1: the renderer's fixed CPU RTV/DSV slots are rewritten
before delayed replay, despite their heaps remaining alive. The facade snapshots
OMSetRenderTargets and clear descriptors on the producer using bounded private
RTV/DSV heaps (4096 command slots, eight RTVs plus one DSV each). Snapshot slots
are released after replay, including failed-command cleanup. When this pool fills,
the producer publishes its partial stream chunk and waits before the next command.
This preserves the descriptor allocator/table logic and contiguous-handle semantics.
It adds descriptor copies in mode 1 and may reduce the achievable saving.

Recorder errors publish an exception before waking waiters. The producer observes
it at its next wait/submit. A failed recorder keeps consuming without executing so
blocked producers can finish publishing and all encoded holds can be released.
Producer Submit exceptions also wake acknowledgement waiters. Debug InfoQueue
read/clear operations serialize across producer error paths and recorder drains.

## Profiling

With `NXBOX_D3D12_FRAME_PROFILE=1`, `D3D12_FRAME_PROFILE` adds:

- `recorder_busy_ms_per_frame`: recorder kernel+user CPU milliseconds divided by
  presented frames, sampled from its native thread handle once per reporting window.
  It excludes time asleep and avoids per-command profiler clocks. The first window
  includes recorder initialization and should be discarded during warmup.
- `recorder_backpressure_waits`: total boundary waits in the reporting window.
- `recorder_backpressure_ms_per_frame`: boundary wait wall time divided by frames.

Backpressure includes both chunk exhaustion and CPU descriptor snapshot exhaustion.
With no presented frames, divisors use one to keep diagnostic values finite. Other
phase fields retain their original units/window behavior; normalize totals by the
reported `frames` as usual. Diagnostic timing is disabled when profiling is off.
The existing executes counter counts producer-sealed work in mode 1; the real queue
acknowledgement remains the fence-manager submission predicate.

## M3 host evidence and limits

The baseline was 218 passing D3D12 port tests. The new test compiles the actual
facade, argument encoder, chunk stream, descriptor snapshot pool and acknowledgement
primitive against a fake list/device in separate ASan+UBSan and TSan binaries.
It checks all 41 facade methods (including the symmetric root-view helpers), exact
call order and arguments, immediate forwarding, signed/64-bit values, null/empty
inputs, contiguous/noncontiguous targets, copy locations, barrier unions, caller
input mutation, COM holds and destruction of a temporary object after encoding.

Additional cases cover 12,288 descriptor snapshots with concurrent replay and slot
recycling, 30,000 records through a three-chunk pool with 31-byte chunks and 1200-byte
payloads, partial flush before waiting, zero producer allocations over 1000 encodes,
failure while an exhausted pool drains, and 96 submissions with acknowledgement
separate from simulated GPU completion. The production CompleteThrough extraction
test now also proves that wrapper leases survive retirement requests beyond the
acknowledged tick. Existing source-extraction harnesses were adapted to the facade
and the new profile fields without removing their behavior assertions.

Commands:

```sh
python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'
python3 -m unittest discover -s tests/port -p 'test_d3d12_deferred_recording.py'
clang++ -std=c++20 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc /tmp/p3-recorder-proof/test.cpp -o /tmp/p3-recorder-proof/asan-ubsan
clang++ -std=c++20 -O1 -g -pthread -fsanitize=thread -fno-omit-frame-pointer -Isrc /tmp/p3-recorder-proof/test.cpp -o /tmp/p3-recorder-proof/tsan
git diff --check
```

The sanitizers exercise the portable production components, not the native Windows
Context, real fences/DRED, DXGI or D3D12 driver. Windows/UWP compilation and all
console behavior remain unverified here. No CI/package ID exists for this change:
the lead must review, commit and build it. No network, console, commit or push
operations were performed.

## M4 expected saving, risks and console check

The supplied warm MK8D scopes total 5.37 ms/frame (draw_recording 3.39, barriers
0.98, submit 1.00); BotW scopes total 25.05 ms/frame (18.3, 3.25, 3.5). These are gross
budgets containing producer work that stays in place, not predicted savings.
Encoding, AddRef/Release, descriptor snapshots and backpressure consume CPU, while
Present's acknowledgement can expose recorder lag. No speedup or FPS projection is
supported by host evidence. Wonder may gain little because of its existing sync waits.

The tester should use a lead-built package and edit `LocalState\nxbox_env.txt`:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_D3D12_DEFERRED_RECORDING=0
NXBOX_D3D12_SUBMIT_TRACE=0
```

Keep all other settings identical. Fully restart the app for each mode (read once).
For MK8D, BotW and Wonder, use identical warm save/scene/controller motion, discard
the first 60 seconds, and capture 60 seconds of steady gameplay. Repeat 0, 1, 0 to
check variance. Save package identity, exact scene, screenshots, diagnostics, FPS,
GPU-thread CPU%, recorder CPU, GPU busy time, draw/execute counts, memory and
`draw_recording`, `barriers`, `submit`, `fence_wait`, `sync_wait` phase values.
Verify the creation log selects the requested mode and includes chunk parameters.
Record all three recorder fields above; nonzero sustained backpressure shows that
the producer caught the recorder rather than simply moving CPU work away.

Success is lower GPU-thread time and improved FPS with no offsetting wait increase,
identical rendering/query-dependent behavior, bounded stable memory, and no hangs,
readback errors or device loss. Also test screenshot capture, resize where supported,
title switching, query-heavy scenes and shutdown after several minutes. If device
loss occurs, preserve `eden_uwp_diag.txt`, `nxbox_crash.txt` and DRED diagnostics;
the producer must receive the recorder failure at its next wait or submit.

Separately compare mode 0 against a package from the unchanged parent revision to
verify the required no-measurable-regression default path. A mode 0 versus 1 A/B
alone cannot prove that property. Main remaining risks are native toolchain/API
compatibility, real descriptor/fence lifetimes, failure/shutdown behavior on a
removed device, recorder scheduling on Xbox, and mode-1 serialization/retention
cost exceeding the D3D12 CPU work removed from the producer.
