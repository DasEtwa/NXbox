# Xbox Series S validation baseline

This document tracks **Series S evidence separately from Series X results**. NXbox is currently experimental; a launch, a menu, or a brief gameplay clip is not evidence that a game is fully playable.

## Scope and rules

- Test a retail **Xbox Series S** in Microsoft Developer Mode with NXbox registered as **Game** (not **App**) in Dev Home. Record the actual App type and memory limit reported by the console; do not assume Series X values transfer to Series S.
- Record the exact NXbox release **and** commit SHA, installation package name, title/update version, firmware version (number only), renderer, graphics settings, storage location, and whether the shader cache was cold or warm.
- Use independently dumped games, firmware, and keys from hardware you own, where lawful. **Never upload copyrighted game files, firmware dumps, keys, or personal credentials to issues or CI.**
- Keep **developer assertions**, **community reports**, and **reproduced measurements** separate. Label every data point with its provenance and a link to public evidence if available.
- Do not present measurements from a Series X, PC, or an emulator's UI as Series S performance.

## M0: build and host checks

1. Confirm the fork's `.github/workflows/build-nxbox.yml` Windows 2025 UWP build and `.github/workflows/port-tests.yml` tests are green before functional changes. Capture workflow URL, source SHA and artifact ID. A green host test is **not** console validation.
2. Record compiler/SDK versions and check whether the UWP package is produced. Document any unsigned/signing or deployment limitations without bypassing the platform's rules.
3. Establish a frozen baseline build; run the same smoke tests before and after each proposed optimization. Keep original project and third-party license notices.

## M1: console smoke tests

Use a short deterministic homebrew workload before trying a commercial title. Test both the default **native D3D12** renderer and fallback **OpenGL-on-Mesa** when supported by the build. If a renderer fails during startup, record that failure separately from a fallback result.

For each workload:

1. Cold start: clear only the **shader cache for the tested title** when safe; record initial boot time and time to first visible frame. Do not wipe user saves or global application data.
2. Reach a reproducible scene; capture a 60-second sample **only if the game remains running**.
3. Repeat with a warm shader cache. Record at least three independent runs where feasible; note sample duration for each run.
4. Check input, audio, save/load (if supported), graphics corruption, stalls, and graceful exit.
5. Capture the relevant `LocalState\\eden_uwp_diag.txt` and `LocalState\\nxbox_crash.txt` (if present); see [crash evidence](crash-evidence.md). Scrub usernames, IPs, tokens, serials, path fragments, and other private data before sharing.
6. Report median FPS and 1% low **only when calculated from actual frame-time samples**; otherwise report a clearly identified on-screen observation, or **not measured**. Include stutters/loading pauses instead of quietly excluding them.

## Test record template

Copy this block into an issue or a test log. Keep one record per combination of game, build, renderer, and console.

```text
Date/time (UTC):
Tester and evidence URL:
Console: Xbox Series S (storage capacity / hardware revision if known)
Xbox OS build:
NXbox release / commit SHA / package filename:
App type (Game/App); reported memory limit:
Test title / title version / update / DLC:
Provenance: homebrew / own dump (no files or keys attached)
Firmware version number:
Renderer: native D3D12 / Mesa fallback
Storage: internal / external; media type:
Resolution, graphics settings, mods:
Cache state: cold / warm / unknown
Repro steps and scene:
Boot-to-first-frame time:
Runs and duration per run:
FPS median / 1% low (method): not measured
Frame-time spikes / stalls:
Peak app memory (measured / unknown):
Result: does not boot / boots / menus / in-game / completed
Graphics / input / sound / save behavior:
Crash or D3D12 device-removal details:
Redacted diagnostic log references:
Uncertainties and next reproduction step:
```

## Initial evidence register

Existing Series S **community reports**, including Sonic Mania, Freedom Planet, and Metroid Dread, are mentioned in the upstream [README](../README.md#compatibility). They are **not converted into verified benchmarks** without the build, test conditions, and independent evidence required above.

| Title / workload | Series S build | Renderer | Evidence | Result | FPS |
| --- | --- | --- | --- | --- | --- |
| Deterministic homebrew smoke test | Pending | Pending | Pending | Not tested | Not measured |
| Sonic Mania (community report) | See upstream report | See upstream report | [Upstream issue #12](https://github.com/NspxMiguel/NXbox/issues/12) | Community-reported success, not reproduced here | Not measured |
| Metroid Dread (community report) | See upstream README | Unknown | Upstream README, Discord attribution | Community-reported gameplay, not reproduced here | Not independently measured |

## Priorities after baseline

1. **Reliability first:** deterministic repros for boot failures, memory exhaustion, black screens, and D3D12 device removal.
2. **Memory headroom:** determine real Series S limits and cold/warm peaks without attributing Series X numbers to Series S.
3. **Correctness before FPS:** fix rendering and input regressions, then profile stalls with a fixed scene.
4. **Performance:** compare changes on the same source SHA, console, renderer, resolution and cache conditions; publish negative results too.

Do not mark a workload **playable** until it has been completed without material defects under explicitly recorded test conditions.
