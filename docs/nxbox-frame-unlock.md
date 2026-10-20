# Experimental VI frame-rate unlock

`NXBOX_FORCE_SWAP_INTERVAL=1` removes ordinary guest presentation waits of 2, 3,
or 4 vblanks on NXbox UWP. A requested interval of 2 becomes 1, allowing a new
buffer every 16.67 ms instead of every 33.33 ms. It does not generate frames,
patch guest instructions, change GPU rendering, or increase emulated clock speed.

Set the variable in the game process environment before launching the title.
`NXBOX_FORCE_SWAP_INTERVAL=0` explicitly disables the override, including future
per-title defaults. Unset uses `FrameUnlockTitles` in
`src/core/hle/service/vi/frame_unlock.h`; that table ships empty. Other explicit
values disable the override. The environment is sampled on the first accepted
buffer and when the application's program ID changes; restart to change it for
the same running title. Non-UWP builds retain their original behavior.

Intervals <=0 and >=5 retain Eden's existing speed-control extension semantics.
Interval 1 is unchanged. Standard frame unlocking never introduces those speed
extensions. A separately enabled extension or emulator turbo can still change
vblank cadence.

## Where the override happens

The producer validates the buffer, then records the original guest request and
stores the effective interval in `BufferItem`, under the existing queue mutex.
Consequently the hardware composer's acquisition gate, composition interval
normalization and buffer release deadline all use the same effective value.
The shared-buffer presentation path also enters this producer. Fences, crop,
transforms, timestamps, pixels and queue ordering are unchanged. This changes
presentation timing intentionally; it cannot promise identical game simulation.

Each queue logs its first accepted interval and subsequent changes, even when
unlocking is disabled and without requiring frame profiling:

```text
NXBOX_SWAP_INTERVAL title=0123456789ABCDEF queue=0x... requested=2 effective=1
```

This is one message per change, not per frame. Queues are identified separately
so concurrent layers cannot hide transitions. `VI_TIMING`'s existing histogram
continues to count **requested** intervals, preserving baseline comparisons.

## Expected performance

For a queue limited only by interval 2, the upper bound changes from 30 to 60
unique presents/s: up to 16.67 ms less presentation pacing per frame. The
production-composer harness confirms 60 versus 120 presents over 120 vblanks.
This is a capacity result with immediately ready buffers, not measured Xbox FPS.
CPU/GPU work per rendered frame is unchanged. Guest sleeps, explicit frame
counters or internal 30 Hz schedulers can keep a title at 30 despite the override.

| Title | Evidence and expected effect |
| --- | --- |
| Wonder | Owner observed dynamic interval 2 and 29.7 FPS. This override removes that presentation wait on each newly queued buffer, including transitions back from interval 1. Candidate for a gain toward 60 if the game can produce frames fast enough. Its simulation behavior in the fallback mode has not been validated here. |
| ACNH | Owner reports a fixed 30 cap. If queue pacing is the limiting mechanism, presentation may rise toward 60, but frame-counted simulation speeds up. Treat a bare unlock as potentially approximately double-speed gameplay, not a correct 60 FPS mode. |
| Naruto Storm 4 | Owner reports a fixed 30 cap. Presentation-only benefit depends on whether this build requests interval 2 or has another internal limiter. Fixed-step/delta-time behavior is unverified; do not enable by default. |
| Naruto Connections | Owner reports a fixed 30 cap. An author-tested guest patch enables a separate internal 60 FPS mode with preserved timing (source below). That does not establish that overriding VI alone selects that mode or preserves speed. Do not enable by default. |
| BotW, TotK, DBSZ, Stray | Baselines 15.6/24/21/23 FPS already fall below 30. Removing a presentation wait cannot alone provide the missing CPU/GPU throughput or repair game logic. No guaranteed improvement. |
| MK8D, Odyssey | Baselines 35/30.4 FPS do not establish an interval-2 cap. A request of 1 remains unchanged; no guaranteed gain. |
| Cuphead, Dread, Arceus, Tennis | Already around 60; no expected gain from a 2-to-1 override if they request 1. |

The supplied last profile windows reinforce the distinction: BotW reports
17.067 ms/frame in recorder busy time alone and MK8D 7.186 ms/frame; these are
nested/concurrent measurements, not additive frame budgets. Wonder's last
window has 157 frames, 4.578 ms/frame recorder busy time and 545.502 ms total
fence-wait wall time. None proves 60 FPS headroom. No `VI_TIMING` lines were
present in these three supplied logs, so their event cadence cannot be inferred
from those captures.

## Game logic speed and delta time

A fixed increment per rendered frame runs twice as often when 30 becomes 60.
Animations, movement, physics, dialogue, combat windows and frame-counted timers
can therefore accelerate even though the emulator clock remains normal. A
system using elapsed emulated time can retain normal speed, but games can mix
both models. A smooth FPS counter is insufficient evidence.

ACNH's [SpeedFix author documentation](https://github.com/tmouh/acnh-60fps)
describes halving per-frame speeds, doubling frame-counted timers and patching
frame-counting code across multiple systems. This supports classifying ACNH as
unsafe for a bare presentation unlock; its patch is version-specific and was not
installed by this change.

The [Connections patch author's release](https://gbatemp.net/threads/naruto-x-boruto-ultimate-ninja-storm-connections-60-fps-mod-v1-7-0.684554/)
reports an internal 60 FPS mode with normal movement, combat and timer speed for
EUR v1.7.0. This is evidence for that guest patch, not for this VI override.
Storm 4 and Wonder remain unclassified. **No listed title is certified here as
fully delta-time-safe under the bare VI override.** That is why the default
title table is empty rather than guessing from the reported FPS.

Before adding a default, compare the same game build with override 0 and 1 at
100% emulator speed: elapsed wall time versus an in-game timer, travel distance,
jump duration, combat windows, dialogue, cutscenes and audio sync. Test both
stable 60 and a workload below 60, plus Wonder's interval transitions. Record
title/build IDs and results; a title-only allowlist is insufficient if later
updates change timing behavior.

## Vblank and vsync verification

The override never touches `Conductor`, its 60 Hz scheduling, or event signaling.
The current hardware composer returns interval 1 regardless of the guest queue
interval, so standard presentation waits do not halve the VI timer. At normal
100% speed with no speed extensions, timer periods remain approximately
16,666,667 ns with the override off or on.

`tests/port/test_vi_frame_unlock.py` compiles the production policy and hardware
composer and checks intervals 1/2/3/4 over 120 vblanks, dynamic request changes,
explicit opt-out, malformed values, title defaults and non-UWP behavior. It also
runs the production conductor and vsync manager with the existing blocked-worker
harness in multicore and single-core modes, with both independent-vsync settings
and both unlock settings, asserting the 60 Hz callback period and signal counts.

This is deterministic code verification, not a Series X runtime measurement.
With `NXBOX_VI_INDEPENDENT_VSYNC=0`, a blocked compositor can still delay guest
signals despite the 60 Hz scheduled callback; this existing limitation is
unchanged. With `NXBOX_VI_INDEPENDENT_VSYNC=1` in multicore, the harness verifies
that signals continue while composition is blocked. Event signals can coalesce
if the guest does not clear the event; signal counts do not mean distinct frames.
Use the existing five-second `VI_TIMING` report on hardware to check roughly
300 ticks/signals per window and inspect maximum lateness/gaps. No guarantee of
60 delivered events/s under overload is made by this presentation override.

Run only this task's checks:

```sh
python3 tests/port/test_vi_frame_unlock.py
```
