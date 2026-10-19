# Metroid Dread EMMI counter timing investigation

## Finding and limits

The supplied evidence does **not establish an emulation bug that can safely be fixed by changing
clock or vsync behavior**. The leading working hypothesis is that guest elapsed time continues while
presentation stalls, compressing the visible counter window and leaving audio behind. This fits the
reported sub-60 performance and memory pressure, but the logs do not identify the QTE itself. A
non-normal `fast_cpu_time` override is the strongest alternative: the code explicitly makes the
counter run 1.7x or 2x faster while presentation can remain capped at 60. Neither setting nor a
clock measurement survives in the supplied Dread logs. Do not describe either hypothesis as
confirmed.

This change adds diagnostic measurements only. It deliberately leaves emulator defaults, guest clock
scaling, VI scheduling, audio buffering and controller mapping unchanged. Run the experiments below
before selecting an emulation fix.

## Evidence read directly

Both directories under `/Users/miguel/.local/share/nxbox/artifacts/asyl-2026-10-18/` were read:
`run1-fp2/` and `run2/`, each containing `eden_uwp_diag.txt` and `nxbox_crash.txt`.

- The run1 diagnostic file (5,133 lines) is an exact prefix of run2's diagnostic file. It covers
  Freedom Planet 2. Run2 adds Mario Wonder. Their `GAME_PRESENT` samples peak at 60.10 FPS, with
  `speed=100.0%`. These are **not Dread FPS samples**.
- The run1 crash file (62,936 lines) is likewise an exact prefix of run2's crash file. The Dread
  session is `event=game title=010093801237C000` at line 47,816 in both files, `tick_ms=3562687`,
  PID 3060. The next game marker is at line 57,325. Thus the two supplied files do not represent two
  independent Dread reproductions.
- Dread's first displayed frame is at line 47,877; frame 63,840 is at line 57,104. Periodic
  `D3D12_FEEDBACK` checkpoints and one-second memory heartbeats suggest both 60 and 30 FPS
  stretches, plus slower stretches. Heartbeat interpolation cannot measure an individual counter
  window or prove a strict instantaneous FPS ceiling.
- Dread's session reaches approximately 5,039 MiB of the 5,120 MiB process limit. There is no QTE
  marker, input-edge trace, audio line, effective timing settings line, or CNTPCT rate in these
  files. The earlier report of 21–29 FPS in area 2 is consistent with pressure, but cannot be
  assigned to the QTE from these logs.

## Code paths and desktop comparison

| Path                                                                                             | What the code does                                                                                                                                                                                                                                                                                         |
| ------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/common/settings.h`                                                                          | Desktop and Xbox share defaults: multicore on, speed limiting on at 100%, `fast_cpu_time=0` (Normal), `sync_core_speed=false`, FIFO vsync, `use_video_framerate=false`.                                                                                                                                    |
| `src/eden_uwp/game_session.cpp`                                                                  | Xbox sets XAudio2, disables fastmem, selects CPU NVDEC and renderer-specific ASTC/shader defaults. It does not set a faster clock or unlock speed. Global `eden_settings.txt`, then `settings/<TITLE>.txt`, can override defaults. The serialized clock label is **`fast_cpu_time`**, not `cpu_clock`.     |
| `src/core/core_timing.cpp`                                                                       | Multicore `GetClockTicks()` starts with wall-clock CNTPCT. Boost multiplies by 1734/1020; Overclock by 2040/1020. `sync_core_speed` divides by the configured speed ratio; at 100% it changes nothing. Global event time and GPU ticks do not receive this CPU-clock multiplier.                           |
| `src/core/arm/dynarmic/arm_dynarmic_64.cpp`, `src/core/hle/kernel/svc/svc_tick.cpp`              | Both guest CNTPCT reads and `GetSystemTick` use that same `GetClockTicks()` path. The advertised Tegra counter frequency is 19.2 MHz, independent of CPU MHz.                                                                                                                                              |
| `src/common/cpu_features.cpp`                                                                    | Native x64 wall time and CNTPCT share calibrated RDTSC scaling; the fallback uses host steady time. Compare both with host steady time to detect a calibration error rather than guessing from FPS.                                                                                                        |
| `src/core/hle/service/vi/conductor.cpp`, `src/core/hle/service/nvnflinger/hardware_composer.cpp` | VI composition controls guest vsync events and honors speed limit, guest swap interval, composition speed scale and optional NVDEC rate. Unlocked multicore schedules much faster. Guest vsync and DXGI presentation are separate mechanisms.                                                              |
| `src/video_core/renderer_d3d12/renderer_d3d12.cpp`                                               | The active native renderer always calls `swapchain->Present(1, 0)`. It does not consult `use_vsync`. Setting `use_vsync=0` is therefore **not** a native-renderer A/B. The separate `d3d12_present/Presenter` helper is not the active call here. A 60 Hz output cap does not prove guest time is correct. |
| `src/hid_core/resource_manager.cpp`, `src/eden_uwp/game_session.cpp`                             | Guest Npad updates are scheduled at 1 ms; Xbox physical polling sleeps 8 ms in the session loop, independently of rendered frames. Work in that loop can delay polling; an overlay suppresses guest input.                                                                                                 |
| `src/eden_uwp/gamepad.h`, `src/eden_uwp/button_layout.h`                                         | X maps to guest X in both layouts. The position option swaps only A/B. Test the counter with physical Xbox X and close the overlay.                                                                                                                                                                        |
| `src/audio_core/renderer/system_manager.cpp`, `src/audio_core/sink/xaudio2_sink.cpp`             | Audio renderer work and the XAudio2 feeder run on separate threads. Four 10 ms sink buffers provide about 40 ms of sink buffering. Rendering stalls and upstream starvation can add delay; the logs do not measure either. Disabling audio is an isolation experiment, not a latency fix.                  |

`GAME_PRESENT speed` comes from global emulated time in `src/core/perf_stats.cpp`; it does not
measure the scaled guest CNTPCT. It can say 100% when `fast_cpu_time` makes guest tick reads run
twice as fast.

## Console reproduction and A/B files

Preserve the existing files and save, use the same Dread build/update/mod configuration and the same
checkpoint, then fully exit and restart NXbox for **every** run. Close the overlay, use physical
Xbox X for the counter and record the screen externally, including sound. Repeat the same attempt
several times, including once after its shaders are warm. Copy both logs immediately after each run,
before another game rotates diagnostics. Note the wall-clock time of each QTE attempt.

First capture an **as-found** run with the new diagnostics and the original settings, without
editing anything. Save all original global and per-title settings and environment files with that
run.

For controlled run **A**, use exactly these timing entries in `LocalState/eden_settings.txt` (retain
unrelated settings). Remove duplicate keys and temporarily move aside
`LocalState/settings/010093801237C000.txt` so it cannot override them:

```ini
renderer_backend=5
use_multi_core=true
use_speed_limit=true
speed_limit=100
fast_cpu_time=0
sync_core_speed=false
use_vsync=2
use_video_framerate=false
```

Use exactly this `LocalState/nxbox_env.txt` for A and B, temporarily saving other environment
overrides:

```ini
NXBOX_RENDERER=d3d12
NXBOX_AUDIO=xaudio2
```

For controlled run **B**, keep the same A settings and environment, changing just:

```ini
fast_cpu_time=2
```

B is a diagnostic positive control, **not a recommended setting**. Expected: `clock_mhz` remains
approximately 19.2 in A and B; `guest_mhz` is approximately 19.2 in A and 38.4 in B. Boost (`1`)
would produce approximately 32.64. If as-found matches B and A restores the window/audio
relationship, remove the faster-clock override for Dread. If A still fails, the positive control
establishes that the clock telemetry is capable of detecting this mechanism; it does not establish
the cause.

The new `TIMING_POLICY` line records effective settings **after per-title application**.
`GAME_TIMING` logs every reporting interval, independently of presents:

- `clock_mhz`: underlying wall-clock CNTPCT versus host steady time. A persistent error here in
  multicore implicates native clock calibration.
- `guest_mhz`: actual guest-visible `GetClockTicks()` rate. A guest-only error implicates
  clock/speed settings. A must be about 19.2 even when rendered FPS falls.
- `polls`, `max_poll_gap_ms`: physical polling calls and their longest interval. Normally expect
  roughly 625 calls per five seconds, with modest scheduling variation. A gap comparable to the
  entire visible window makes missed input plausible. This measures calls, not Windows input
  delivery or guest HID consumption. Resume resets the interval so suspension is not a false hitch.

Then, if A's clocks are normal, compare **A against C**, retaining A's settings and changing only
the environment file to:

```ini
NXBOX_RENDERER=d3d12
NXBOX_AUDIO=null
```

If the window/input becomes correct only in C, investigate audio scheduling/backpressure next. C has
no audible output, so judge the window using the external video. If both fail, restore A and compare
cold versus warm attempts; correlate `GAME_STALL`, frame gaps, polling gaps and memory with the
attempt timestamp. A normal guest counter plus a large presentation gap supports the leading
hypothesis. Normal clocks and normal polling with a bad QTE still require a guest input/VI trace;
these measurements cannot prove that the game consumed a button edge. Do not change clock scaling,
VI catch-up, HID frequency, or audio queue length globally based only on this report.

Restore the preserved settings/environment and per-title file after the experiments.

## Validation

`python3 -B -m unittest discover -s tests/port -p 'test_guest_timing.py'` executes the diagnostic
arithmetic with host clang++, including large epochs, normal/doubled clocks, input stalls and resume
reset. It also checks production wiring. Before the implementation it failed with one missing-header
error and one missing-wiring failure; afterwards: `Ran 2 tests`, `OK`.

Xbox/MSVC build, actual Dread counter timing, audio latency, and the console A/B/C outcomes have not
been verified on this Mac. No claim that the reported QTE is fixed is made.

Additional checks (all commands use `python3 -B -m unittest discover -s tests/port -p`):

| Pattern                           | Output                                                                                                   |
| --------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `test_title_settings.py`          | `Ran 7 tests` / `OK`                                                                                     |
| `test_crash_format.py`            | `Ran 3 tests` / `OK`                                                                                     |
| `test_input_state.py`             | `Ran 3 tests` / `OK`                                                                                     |
| `test_renderer_settings_async.py` | `Ran 2 tests` / `OK`                                                                                     |
| `test_button_layout.py`           | Import failed: `ModuleNotFoundError: No module named 'fontTools'`; this existing suite was not executed. |

`git diff --check` passed. No console experiment was run and no changes were pushed.
