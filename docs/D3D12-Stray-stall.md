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
