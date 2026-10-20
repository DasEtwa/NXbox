# DBSZ: the 43.822-second presentation gap

Source: read-only `compat-2026-10-18/q-dbsz-np/{eden,diag,crash}.txt`,
examined 2026-10-20. Window indices here are zero-based `GAME_PRESENT` indices.
No console rerun was performed. This investigation does **not** establish a
runtime fix for the gap.

## Timestamp chain

| Evidence | Location | Observation |
| --- | --- | --- |
| Last presentation before gap | eden.txt:228–230 | Begin 29.765257 s, complete 29.767403 s, frame 2 |
| Work continues | eden.txt:231–265 | Submissions at 35.20–35.25 s; guest service calls at 38.22 and 40.58 s |
| Mapping counter assertion | eden.txt:266 | At 45.114645 s, `current_block != 65535` in `Memory::MapPages` |
| Guest initialization resumes visibly | eden.txt:267–268 | Copyright framebuffer initialization and gameplay recording state at 71.979535 s |
| Next presentation | eden.txt:277–279 | Begin 73.581961 s, complete 73.583957 s, frame 3 |
| Gap reported | diag.txt:439 | Window 11, tick 94215796, `worst_gap_ms=43822`, two frames |

Presentation completions are 43.816554 s apart, within milliseconds of the
frontend's 43.822 s measurement at a different call site. This is the early
initialization/loading interval after three frames, not a late gameplay cache
eviction. No `Present begin` remains unmatched throughout those 43 seconds.

## What can be excluded, and what cannot

**Capacity eviction is excluded for this gap.** All clear counters are zero
through window 13. Window 11 ends with core occupancies
162,427,545 / 176,635,323 / 40,768,467 bytes against 201,326,592 bytes per core.
Sampled repeat compilations and range/fastmem invalidations are also zero.
The actual two capacity clears occur in windows 14 and 20 (diag.txt:686 and
1174), each taking 175 ms, discarding 389,835 and 382,598 descriptors respectively.
Their window maximum presentation gaps are 495 and 720 ms. They cause later
recompilation, but cannot cause an earlier 43-second interval.

Windows 4–11 sum to 6,834 ms inclusive JIT time (277,574 completed miss handlers),
including 5,118 ms emission; I/O totals 3,573 ms and decommit 239 ms. Including
the entire boundary window 3 gives 9,641 ms JIT and 3,621 ms I/O. These are
cross-core elapsed scopes, include work outside the exact gap, and overlap;
they neither add up to the gap nor account for guest execution or guest sleeps.
An operation still in progress does not appear in completed-scope totals.

**The logged PSO prewarm is outside the gap:** eden.txt:97–98 records
4.455086–13.584967 s, 246 records, 9,370 ms, zero DXC time. Its 35,212.884 ms
driver total is across four workers, not a 35-second wall wait.
`NXBOX_D3D12_FRAME_PROFILE=0` is explicit at diag.txt:15. Missing per-frame PSO
and fence timing is not proof of zero time. In particular, `shader=0` does not
rule out every D3D12 compile path. No single blocking GPU/PSO call is identified
by this capture. Older `w-dbsz` evidence is discussed separately in
[the submission investigation](D3D12-DBSZ-submission-pressure.md); it cannot
provide exact timing for this run. The supplied `r-dbsz*` path pattern has no
matches in the artifact directory available here.

**Memory pressure is not demonstrated as the cause.** Window memory rises from
2,344,267,776 bytes in window 3 to 3,903,934,464 in window 11, below the
5,368,709,120-byte cap. One-second heartbeats continue, with memory level 0
then 1 during the gap, and no device loss. The ~220 MiB late-run headroom is
not the headroom at this early gap. Backing-page demand commits, guest memory
initialization and ordinary guest work are not individually timed here;
headroom alone cannot exclude their cost. The mapping-counter assertion is a
real separate observation, not evidence of a 26-second block: the source uses
an atomic 16-bit mapping tag and the capture continues after the assertion.

**Guest progress and sleeps are visible.** diag.txt:157–438 repeatedly dumps
guest threads. Thread 82 changes PCs, then appears in windows 7–10 at
`pc=0x8c5c8a0c`, `lr=0x828220e8`, `state=1 wait=1`. The enum in
`k_thread.h` identifies this as a SleepThread wait. Thread 124 changes from
runnable code at `0x82817f20` to condition-variable waits, then runnable code
at `0x828acca4`. These saved contexts are samples, not execution stacks or
durations. They support guest-side loading/waiting, but do not identify the
sleep's requested duration, the dependency being polled, or a broken timeout.
`GAME_TIMING` stays at 19.2000 MHz for both clocks; frontend polls continue with
maximum gaps of roughly 55–65 ms in the middle of loading. There is no evidence
of a globally stopped host clock or a 43-second frontend freeze.

## Fix decision and next capture

No runtime cache, memory-budget, timeout or renderer policy was changed.
Partial eviction would not affect this gap and requires safe unlinking of
incoming branches, dispatch/RSB entries, fastmem records and code storage before
reuse. Dropping descriptors alone does not reclaim the arena. Increasing cache
capacity or retaining an extra copy of hot blocks also violates the requested
memory constraint without addressing the measured interval.

The precise blocking operation remains unproven. The smallest useful next step
is to use the **existing** `NXBOX_WAIT_DIAG=1` recorder with
`NXBOX_D3D12_FRAME_PROFILE=1` and `NXBOX_THREAD_PROFILE=1`, preserving title,
save, input sequence and cache warmth. This is an instrumented attribution run,
not a no-profiler performance comparison. No new runtime storage was added by
this investigation; enabling the existing recorder itself has bounded overhead.

The next capture should contain `WAIT_ENTRY`, `WAIT_PROGRESS` and
`WAIT_PENDING_SUMMARY` during zero-frame windows. Correlate SVC `SleepThread`
entries (guest thread key 82 / 0x52) and their nanosecond argument and duration
with the caller LR above; inspect service entries for worker dependencies.
Increasing age of the **same active token** distinguishes one blocked call
from repeated sleeps. Fence ages and submitted/completed ticks distinguish a
GPU dependency, while driver compile timings and compiler-thread CPU distinguish
PSO work. Check `dropped` before assuming a missing entry means no wait.
If these show short sleeps and advancing workers with no long host wait, the
remaining target is guest loading work; a host/guest PC profile is needed to
locate it. There is no evidence-backed promise that the next run will have a
shorter gap or higher FPS.

Validation: `python3 -m unittest discover -s tests/port -p test_wait_diagnostics.py`
passes all four existing recorder tests, including snapshots while the producer
is stopped and cross-core completion. These validate diagnostic mechanics, not
the cause of the console gap. Only this document was added; no commit was made.
