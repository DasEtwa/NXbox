# Wonder vblank timing investigation

## Measurements

Analyzed the supplied `compat-2026-10-18` recordings. These are historical
measurements, not results from this change. `winstat.py` reports 34 live windows
per Wonder run, median game FPS 30.0, and GPU-thread active wall-time shares of
72% (`w`) and 71% (`z`). That share is **not native GPU utilization** or thread
CPU utilization. The gameplay segment selected by `perfcmp.py` has 19 windows:

| Recording | Median game FPS | Minimum | Maximum |
| --- | ---: | ---: | ---: |
| `w-wonder` | 29.9 | 24.7 | 37.2 |
| `z-wonder` | 29.9 | 23.1 | 38.6 |
| `x-botw-rec` | 12.9 | 0.7 | 29.9 |
| `x-ody-rec` | 24.7 | 22.8 | 40.6 |
| `x-mk8d-rec` | 31.0 | 25.7 | 44.8 |
| `dbsz-final105` | 18.8 | 0.1 | 39.5 |

Wonder is close to 30 FPS, but `game_fps` is not exactly 30.0 in every window.
Its guest and underlying clocks both report 19.2000 MHz. At +80 to +140 seconds,
`tprof.py` reports 0.23/0.22 ms per Present call, 873.9/852.4 ms median host
fence wait per five-second window, and 57.0%/53.3% GPU-thread CPU use for `w`/`z`.
PSO compiler CPU medians are zero.

Largest relevant costs in the final 12 frame-profile windows, weighted by
their frame counts, in milliseconds per frame:

| Recording | Native GPU | Maxwell methods | Fence wall wait | Recorder busy | Texture update | Buffer binding |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `w-wonder` | 8.258 | 7.584 | 3.998 | 3.751 | 1.906 | 1.634 |
| `z-wonder` | 8.253 | 7.616 | 4.034 | 3.708 | 1.878 | 1.646 |
| `x-botw-rec` | 28.090 | 9.836 | 0.356 | 15.187 | 6.171 | 6.057 |
| `x-ody-rec` | 11.725 | 13.282 | 0.001 | 5.484 | 2.278 | 2.379 |
| `x-mk8d-rec` | 9.475 | 5.806 | 0.149 | 5.726 | 2.399 | 2.687 |
| `dbsz-final105` | 11.321 | 5.535 | 0.093 | unavailable | 2.528 | 4.039 |

These scopes can overlap or nest; do not add the columns. Wonder also spends
3.495/3.542 ms per frame in sync-wait scopes and 2.069/2.002 ms in descriptor
setup. Its final 12 guest-wait windows have median queued-fence latency of
30.471/30.799 ms versus 1.156/1.141 ms execution latency. Earlier steady windows
show roughly 24 ms queued latency. This motivates investigating notification
and queue dependencies, not assuming that spare native GPU time proves 60 FPS.

The DBSZ file is named `eden_uwp_diag.txt`, not `diag.txt`. The first combined
`perfcmp.py` invocation failed on that nonexistent filename; rerunning with the
actual filename produced the DBSZ row above.

## Source findings and change

- Guest `QueueBufferInput::swap_interval` is copied into `BufferItem`. The
  composer uses it to hold/release buffers and gate acquisition. No HLE
  `SetSwapInterval` ioctl was found; this queue field is the guest-facing path.
  The existing logs do not record its value, so an interval of two is unproven.
- `HardwareComposer::ComposeLocked` currently advances one frame and returns
  one. At 100% speed, `Conductor::GetNextTicks` therefore schedules composition
  callbacks every approximately 16.667 ms. `CoreTiming::Advance` reschedules
  against the previous deadline, not completion time. No unconditional
  33.333 ms timer was found.
- However, that callback only wakes `VSyncThread` through a coalescing event.
  The worker calls `ComposeOnDisplay` **before** `SignalVsync`. Composition
  calls `nvdisp_disp0::WaitForComposite`, which waits for the previous GPU-thread
  composite operation. Consequently the guest's VI event is delayed by that
  wait, and several timer callbacks can collapse into one notification. A guest
  waiting for vblank can miss notifications while rendering work is still queued.
- The actual D3D12 renderer and the optional presenter use `Present(1, 0)`.
  The swapchain has two buffers, no frame-latency waitable-object flag, and no
  wait on such an object. There is a separate GPU fence wait when reusing each
  buffer; short Present calls alone do not rule out those stalls.
- `SpeedLimiter::DoSpeedLimiting` immediately returns in multicore mode.
  XAudio2 feeds 10 ms buffers with four queued buffers; no explicit 30 FPS
  presentation limiter was found there. Dynamic audio backpressure is not
  excluded by this source inspection.

`NXBOX_VI_INDEPENDENT_VSYNC=1`, checked through `Service::VI::BisectEnabled`,
moves the VI notification to the CoreTiming callback in multicore UWP builds.
Composition remains on the existing worker, with its existing backpressure.
The worker does not send a duplicate notification. Vblank uses the base refresh
interval even if composition reports a larger swap interval; existing speed
limit, video and composition-speed extensions are retained. Guest buffer swap
intervals, acquisition/release rules, host swapchain settings and audio pacing
are unchanged.

The switch defaults **off**: it changes notification ordering for all UWP
games, can change guest scheduling and buffer availability, and has not been
compared with hardware vblank traces. It removes a demonstrated dependency;
it does not establish that this dependency alone causes Wonder's 30 FPS.
There is no claim of a measured console speedup or locked 60 FPS on Series S.

The event registry has its own mutex so notification and unlink/destruction
cannot race. Neither notification nor registry access takes the container lock
held during composition. Pacing values shared between the timer and composition
threads are atomic. The worker is explicitly joined before its pacing and
synchronization members are destroyed, and exits after a shutdown wake.

## Console A/B

Keep the route, cache and other settings identical and run with:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_THREAD_PROFILE=1
NXBOX_VI_INDEPENDENT_VSYNC=0
```

Repeat with `NXBOX_VI_INDEPENDENT_VSYNC=1`. Both modes now emit `VI_TIMING`
alongside the existing five-second report:

- `ticks`: timer callbacks; `signals`: broadcast attempts, not guest wakeups;
  `compositions`: completed worker passes across the display list.
- `late_max_ms`: maximum CoreTiming callback lateness.
- `signal_gap_max_ms`: largest gap between broadcast attempts. Pause/resume
  gaps can appear here; exclude those windows. The first signal of a new system
  establishes a fresh baseline.
- `compose_ms` / `compose_max_ms`: cumulative / maximum worker-pass wall time,
  including composite backpressure and all displays.
- `swap_negative`, `swap_0` through `swap_4`, `swap_extended`: histogram of raw
  accepted buffer intervals across producers, before normalization. Overlays
  and applets contribute too; the histogram is not per-layer attribution.

Telemetry uses relaxed atomic counters; concurrent report boundaries are
approximate. The timer thread never writes the diagnostic file. Counters are
enabled only by the existing profile switches, independently of the timing
experiment. Single-core builds retain their original composition/notification
order and report their own callback/composition counts.

If legacy `ticks` is near 300 per five seconds but `signals` falls behind while
composition is blocked, the notification dependency is observable on console.
The experiment should bring signals back toward ticks. If signals stay near
300 and `swap_2` dominates in both modes, investigate the guest's interval
selection rather than forcing interval one. If `late_max_ms` is large in both
modes, investigate HostTiming scheduling; this change does not repair timer
thread starvation. Compare game FPS, fence latency and rendering correctness
as well as event counts.

## Validation

The blocked-compositor regression compiled the production `conductor.cpp` and
`vsync_manager.cpp` against host service stubs. Before the change it failed:
`FAILED (errors=1)`, with the assertion that the first timer tick must notify
the guest even while composition is blocked. Afterward it passes with exact
opt-in, unset/zero/invalid switch values, and single-core mode. It also checks
unlink/relink while blocked, no duplicate worker notifications, and the
approximately 16.667 ms callback period. Separate tests exercise concurrent
interval accounting, lateness versus composition costs, report reset/new-system
reset and the producer/reporter wiring.

Commands and final output:

```text
python3 -B -m unittest discover -s tests/port -p 'test_vi_vblank.py'
Ran 3 tests
OK

python3 -B -m unittest discover -s tests/port -p 'test_guest_timing.py'
Ran 2 tests
OK

python3 -B -m unittest discover -s tests/port -p 'test_d3d12_backend.py'
Ran 2 tests
OK
```

Clang-format was applied to the new header and changed C++ ranges only. Ruff
format/check and `git diff --check` passed.

Not verified: MSVC/UWP build and warning policy, actual guest interval choices,
hardware versus emulated vblank timestamps, Xbox event/scheduler behavior,
audio/visual correctness, other games with the switch enabled, and console
performance or Series S 60 FPS. Host tests block a mock compositor; they do
not run the game or D3D12.
