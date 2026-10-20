# Stray: stalled guest, October 18 capture

Title `010075101EF84000`, update v0.2.0. Evidence is the read-only
`artifacts/compat-2026-10-18/u-stray/{eden,diag,crash}.txt` capture and `shots/`.
No console replay or Windows/UWP build was performed on the Mac.

## What the capture establishes

- `eden.txt`: the last sampled presentation completes at 121.150020 s; frame
  1800 feedback is logged at 121.246598 s. These are sampled messages, not the
  final actual frame.
- Syncpoints still retire at 121.843207 s (`pending=0 signaled=21480`). Two
  new pending increments are logged immediately afterward. The logger only
  emits selected values (first eight, then multiples of 120); this is **not**
  evidence that two syncpoints remain pending throughout the stall.
- The final sampled command list is sequence/tick 39720 at 121.964316 s.
  With deferred recording enabled, this message describes sealing the list,
  not acknowledgment by the recording thread or completion by the GPU.
- The last decode enqueue message is at 121.973131 s. There were also decode
  bursts well before the failure. No decoder-start/completion/retirement
  records exist in this capture.
- `diag.txt` lines 2337, 2438, 2496 and 2554 show successive five-second
  presentation windows of 116, 81, 0 and 0 frames. The corresponding host ticks
  are 69201109, 69206125, 69211125 and 69216140 ms.
- The mixed window has GPU/recorder/fencing CPU usage of 30.31/9.38/2.19%.
  In the following window these threads and ImageTranscode workers are at
  0%; host present/fence wait accounting is also zero. FS, nvservices and
  audio are idle. CPUCore_0 and CPUCore_1 still do work. Zero CPU usage cannot
  distinguish an idle thread from one blocked on a lock/event. Wait accounting
  reports completed scopes, so it cannot exclude a wait still in progress.
- At 147.379462 s thread 0x54 on CPUCore_0 writes to address 3 at PC
  `0x80e98fe0`, LR `0x80e990d0`. This is consistent with a deliberate fatal
  path; it does not identify its original trigger. The retained 32 SVCs are
  completed SleepThread calls with 500 ms arguments. The count 263 is that
  thread's lifetime SVC count, not a count of sleeps during the silence.
  In particular, 250 serial 500 ms sleeps would take 125 s, not 25 s.
  The same fatal write repeats at 172.430219 s and 197.478224 s, approximately
  25.05 s apart. Thus the sleep loop could be fatal-handler retry/backoff,
  rather than polling an outstanding renderer operation. This is an inference,
  not a decoded guest call stack; the capture does not establish what it polls.
- The crash heartbeat continues, with no device loss reported and process
  memory around 4.35 GB against a 5.37 GB limit. The 190 s screenshot still
  shows the game scene. Neither establishes the guest's fatal error text.

## Code audit and conclusion

`QueueAsyncDecode` unswizzles into owned input, queues conversion on
`texture_decode_worker`, and publishes `complete` under the decode mutex.
`TickAsyncDecode` uploads completed work, clears `IsDecoding`, handles cancelled
images and erases contexts after unlocking their mutex. `TextureCache::TickFrame`
drives it. D3D12 calls `rasterizer.TickFrame()` from `RendererD3D12::Composite`,
which calls `texture_cache.TickFrame()`. Retirement is therefore wired up during
presentation; it is not absent from the backend. A completed decode left behind
once all presentation stops can be a consequence of the stall.

Fence batching was **off** in this capture. The D3D12 fence manager submits a
native fence for guest progress and retires it on its independent fencing thread.
Query reports use that fence path. No logged timeout or failure in this capture
identifies a particular fence, report, event or service request as the blocker.
The existing detailed counters are emitted with renderer work and disappear when
that work stops. There is insufficient evidence for a root-cause behavior change.

## Next console run

Use a build containing the wait diagnostics, add this line to the console's
`LocalState/nxbox_env.txt`, and restart NXbox before reproducing:

```ini
NXBOX_WAIT_DIAG=1
```

Keep the other renderer settings identical. This switch is cached on first use;
it must be present before the title starts. Capture `diag.txt`, `eden.txt` and
`crash.txt` through the first fatal write. The new heartbeat emits `WAIT_PROGRESS`
every five seconds and `WAIT_SNAPSHOT` / `WAIT_ENTRY` when a whole presentation
window has zero frames. It reads independent copies and never queues GPU work,
walks guest thread lists, or acquires guest/kernel/renderer locks.

Counters are process-lifetime totals (ticks are maxima); compare adjacent windows.
Counter fields are independent atomic observations, not a transactional snapshot.
A transient mismatch is normal. `completed_observed_tick` is the last completion
seen by an emulator wait/collection, not a fresh hardware fence poll.

| Persistent signature | Interpretation and next evidence |
| --- | --- |
| `decode_dispatched > decode_started` | Conversion work was accepted but not started; inspect the decoder worker queue. `decode_queued > decode_dispatched` instead locates the gap before/inside dispatch, including unswizzle. |
| `decode_started > decode_completed` | Conversion is unfinished. Compare ImageTranscode CPU activity and the growth across windows. |
| `decode_completed > decode_uploaded + decode_cancelled` | Converted output awaits retirement. Check `decode_ticks`: if ticking stopped with presentation, this alone does not prove a cause. If ticking continues without retirement, inspect upload/locking. |
| `sealed_tick > submitted_tick` | The recorder has not acknowledged the last sealed work. An active `fence` entry names its tick and wait site. |
| Submitted work, active `fence` with growing duration | A wait is still in progress. Its `args[0]` is the target tick; compare observed completion and device-loss diagnostics. |
| `syncpoint_queued > syncpoint_signaled`, active `nv_event` | Event `args` contain syncpoint ID, threshold, timeout, event slot and minimum at arm time. Compare GPU progress; an event stays active until signal or cancellation. |
| Active `syncpoint` with growing duration | A host/guest syncpoint wait has not returned. `key` is the syncpoint ID; `args[0:2]` are threshold and observed value at entry. This also covers the blocking NV fallback after repeated event failures. |
| `query_queued > query_written + query_invalidated` | Guest query callbacks are outstanding. Correlate with native tick/fence and syncpoint progress; this narrows the query/fence retirement path, but is not proof that the guest reads that report. |
| Active `service` with increasing duration | A service handler has not returned. `key` is the requesting guest TID, `label` is the service, `args[0:2]` are command type and command ID. Capture starts before the service lock, so lock contention is included. |
| Active `svc` | `key` is guest TID, `label` is the SVC number **in decimal**, `args` are entry x0..x7, PC/LR identify its call site, and `result` is return x0 for a completed call. SVC 33 is SendSyncRequest; 24 is WaitSynchronization; 11 is SleepThread. Match service entries by TID. |
| Repeated SleepThread at the same call site with increasing token | Distinguishes a polling loop from one long blocked SVC. Compare other guest TIDs to locate the producer it waits for. A stale completed entry is only the last SVC, not a current guest PC sample. |

`active=true` means the instrumented operation has not returned/signalled. The
snapshot reports age and duration in host monotonic milliseconds, including when
a guest thread migrates cores. `dropped` must remain zero; nonzero means bounded
storage (256 rows) lost observations. Service scopes that defer their replies can
finish while the guest's IPC SVC remains blocked; correlate both kinds of row.
With the switch enabled, fault SVC history also records host entry/return times
and PC/LR. Its 128-entry ring retains more than a minute of serial 500 ms sleeps,
so the next dump can show what preceded the first 25 s interval. Zero timestamps
mean timing capture was disabled.

The fault dump also follows up to 16 aligned, mapped AArch64 frame records
(`NXBOX FAULT FRAME`) to expose callers above the deliberate crash helper. It
stops on missing memory, backward links or a frame gap over 1 MiB; omitted frame
pointers can still prevent unwinding. The recorder identifies outstanding host
operations, but cannot decode an
arbitrary guest userspace lock or recover UE4's fatal message by itself. If all
host operations finish, PC/LR and the polling thread's peers become the next
starting point for guest-code inspection.

## Validation

`tests/port/test_wait_diagnostics.py` executes the production recorder with a
stopped frame producer, independent decoder completion, guest-thread migration,
late completions, cancellation, monotonic ticks, bounded overflow, a real blocked
`SyncpointManager::WaitHost`, and disabled
instrumentation. The existing bookkeeping harness includes the production
recorder header because it compiles `Context::CompleteThrough` directly. The focused
run of wait diagnostics, D3D12 bookkeeping, fence manager and GPU pipeline tests
passed all 19 cases. The fatal frame walk is exercised with AddressSanitizer and
UndefinedBehaviorSanitizer, including malformed and missing frame records.

The requested `python3 -m unittest discover -s tests/port` run completed with
880 tests, zero failures, 44 skips and only the two environmental `fontTools`
import errors (`test_button_layout`, `test_xbox_button_icons`). The focused
19-case run includes the later-added fatal frame-walk test. `git diff --check`
and Ruff checking of the new test file passed. Changes are uncommitted.

## Follow-up: `r-stray` wait-diagnostics capture

Read-only evidence: `/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/r-stray/`
(`diag.txt`, `eden.txt`, `crash.txt`, and both screenshots). Line references below
refer to those files, not the earlier `u-stray` run. This capture narrows the
investigation but **does not identify the root cause or the responsible
signaller**. No emulator behavior change is justified by the evidence alone.

### What actually remains active

`diag.txt:2460` is the first snapshot after presentation stops:
`sampled_ms=92223449`, 84 entries, `dropped=0`. All 18 stall snapshots through
`diag.txt:5117` (`sampled_ms=92308506`) have **zero active service, fence,
NV-event, or syncpoint entries and zero active SendSyncRequest (SVC 33) entries**.
This rules out a persistent wait *inside those instrumented scopes*, not an
uninstrumented lock or guest userspace polling loop.

Several guest threads do remain in SVC **28 decimal / 0x1c,
WaitProcessWideKeyAtomic**, with an infinite timeout. Examples from the first
stall snapshot:

| Guest TID | Mutex address (x0) | Condition-variable key (x1) | Tag (x2) | Host entry ms | Age at first snapshot |
| --- | --- | --- | --- | --- | --- |
| `0x55` | `0x11c0b59d0c` | `0x11c0b59d10` | `0x1601d8` | 92213519 | 9930 ms |
| `0x56` | `0x11c0b59cdc` | `0x11c0b59ce0` | `0x1681d7` | 92213486 | 9963 ms |
| `0x66` | `0x11c0b59e8c` | `0x11c0b59e90` | `0x2401bd` | 92213497 | 9952 ms |
| `0x6a` | `0x11c0bf5d9c` | `0x11c0bf5da0` | `0x2a01b1` | 92213496 | 9953 ms |
| `0x79` | `0x11c0b59e2c` | `0x11c0b59e30` | `0x3581a0` | 92213521 | 9928 ms |

Sources: `diag.txt:2470-2471,2489,2496,2515`. Each listed token is unchanged in
the final snapshot: these calls have not returned after approximately 95 s.
The last completed sleep before the first fatal ends at host ms 92238936
(`eden.txt:1435`), placing the listed waits at approximately **25.4 s old at
the fatal**. That is an age at the fault, not a completed wait duration.
There are other long-lived condition-variable waits, including ones predating
the stall; the table does not establish which thread is the critical consumer.

The ABI and `svc_condition_variable.cpp` identify these as guest condition
variables, not NV event handles, syncpoint IDs, or native fence ticks. A guest
caller of `SignalProcessWideKey` on the matching key normally wakes them;
mutex reacquisition can also be involved. Neither the caller's identity nor the
mutex word/owner or signal history is in this capture. The last `nvdrv` service
calls for TIDs `0x66` and `0x6a` were IPC command 1 (Ioctl1), completed in 0 ms
(`diag.txt:2500-2501`). No ioctl selector is retained in these service rows.
Adjacency is insufficient to call these condition variables NV waits.

### Native progress and the missing retirement observation

`diag.txt:2397,2459` and every later progress row retain:

- Decode queued/dispatched/started/completed/uploaded: **221 each**, cancelled 0.
  `decode_ticks=1636`. There is no outstanding decode in these totals.
- Sealed/submitted tick **37410**, last observed completion **37408**. Both final
  lists were acknowledged by the recorder. Completion is an observation made by
  emulator collection/wait code, not an independent hardware poll.
- Syncpoints queued/signalled **19676/19675**; queries queued/written/invalidated
  **58093/58014/0**. One increment and 79 report callbacks are unretired, but
  aggregate counters contain neither their IDs/thresholds/addresses nor the
  native tick to which each belongs.
- NV events armed/signalled/cancelled **72/70/2**: no outstanding recorded arm.

The fencing host thread is TID **2892 / 0xb4c** (`THREAD_PROFILE`). Its last
native fence wait (`diag.txt:2488`) targets **0x9220 / 37408**, begins at host
ms **92213499**, lasts **2 ms**, and is **completed**. The GPU host thread
TID **4612 / 0x1204** last completed `recorder timestamp reuse` for
**0x9212 / 37394** in 0 ms (`diag.txt:2490`). All later snapshots retain these
completed entries. There is no recorded long native fence wait to blame.

The last recorded NV event on syncpoint **1**, threshold **0x4c6d / 19565**,
timeout **100**, event slot **0**, minimum at arm **0x4c6c / 19564**, completed
in **1 ms** (`diag.txt:2497`). The recorded host wait on syncpoint **3**,
threshold **0x141**, observed **0x145**, completed in 0 ms (`diag.txt:2542`).
Neither is an outstanding dependency at the fatal.

`D3D12::FenceManager::SignalSyncPoint` delegates host signalling to
`VideoCommon::FenceManager::ReleaseThreadFunc`. After `WaitFence` returns,
that thread calls `PopAsyncFlushes`, runs guest callbacks, and pushes the fence
into the destruction ring. Those steps, including acquisition of cache locks,
are **outside** the native `WaitSubmitted` diagnostic scope. Consequently the
completed tick-37408 wait does not tell us whether the fencing thread is idle,
blocked in cache retirement, or blocked in a callback. Its 0% CPU usage does
not distinguish those cases. The same limitation applies to the GPU thread's
uninstrumented locks. No stack dump in these artifacts resolves this gap.

### Fatal thread and clock domains

The first fatal is at **133.871731 s**, TID **0x54**, PC **0x80c45fe0**,
LR **0x80c460d0**, writing **0xe0000002** to address **3**
(`eden.txt:1247-1282,1437-1441`). The frame-record return PCs are
`0x80be53fc`, `0x86efa7a0`, and `0x86f006a4`; no guest symbols or instruction
bytes for these callers are supplied. These addresses do not identify a named
UE4 subsystem by themselves.

The retained 128 SVC records are all completed **SleepThread(500000000 ns)**
calls from PC **0x86f27734**, LR **0x80c46108**, covering host ms
**92174495..92238936** (`eden.txt:1307-1435`). More decisively, the *same loop*
already appears in `diag.txt:260,497,613`, before sustained gameplay. This
thread is not shown entering a 25-second blocking SVC when presentation stops.
A watchdog is a plausible interpretation, not a decoded identification.
Increasing its timeout or force-signalling a guest condition variable would
hide a symptom without identifying the producer failure.

Do not directly subtract `GAME_PRESENT tick_ms` / crash heartbeat `tick_ms`
from `WAIT_SNAPSHOT sampled_ms` or SVC `entered_ms`: the former uses Windows
`GetTickCount64`, the latter `steady_clock`. Their origins differ by about
51 seconds in this run. Wait ages and durations above use the recorder's clock
only. `GAME_PRESENT` windows go from 112 frames (`diag.txt:2336`) to 1
(`2398`) to 0 (`2545`). The 190-second screenshot retains a scene with the cat;
it supplies no fatal message. Heartbeats report no device loss.

### What is required to establish a fix

The missing link is the producer of the guest condition-variable/polling wait
and the current operation of the host GPU/fencing threads. A host stack dump
during the freeze would distinguish cache-lock/callback retirement from an idle
queue. Exact guest executable bytes/symbols and current peer-thread PCs/stacks
would identify the waiting game subsystem and its producer. Alternatively,
diagnostics must cover fence publication, cache retirement, guest callbacks,
and forced drains, and retain resolved guest wait objects plus condition-variable
signal/ownership history. Repeating this build's counters alone will not supply
those missing relationships.

The capture does not establish a renderer, audio, filesystem, nvdrv, timer, or
kernel-threading defect. No root-cause patch or root-cause regression test is
claimed for this follow-up; an invented signaller or synthetic test of a guessed
failure would not close the evidence chain.

Follow-up validation: `python3 -m unittest discover -s tests/port` ran **894
tests in 395.641 s**, with **zero failures, 44 skips, and two environmental
`fontTools` import errors** (`test_button_layout`, `test_xbox_button_icons`).
`git diff --check` passed. Only this report was changed; no console replay,
emulator patch, new regression test, or commit was made.
