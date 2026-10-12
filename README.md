<!--
SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
SPDX-FileCopyrightText: 2018 yuzu Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

<p align="center">
  <img src="docs/assets/nxbox-banner.png" alt="NXbox: Nintendo Switch to Xbox Series X" width="100%">
</p>

<h1 align="center">NXbox</h1>

<p align="center">
  <b>A Nintendo Switch emulator that runs natively on an Xbox Series X.</b><br>
  A port of <a href="https://git.eden-emu.dev/eden-emu/eden">Eden</a> to the Xbox UWP sandbox: the Switch's ARM64 code runs
  through a JIT, and its graphics go to Direct3D 12: today through OpenGL on Mesa, and, <b>*New*</b>, through a
  native Direct3D 12 renderer.
</p>

<p align="center">
  <a href="LICENSE.txt"><img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue"></a>
  <img alt="Platform: Xbox Series X|S (Developer Mode)" src="https://img.shields.io/badge/platform-Xbox%20Series%20X%7CS-107C10?logo=xbox&logoColor=white">
  <img alt="Status: experimental, no game playable yet" src="https://img.shields.io/badge/status-experimental%20%E2%80%94%20no%20game%20playable-orange">
  <a href="https://github.com/NspxMiguel/NXbox/actions/workflows/build-nxbox.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/NspxMiguel/NXbox/build-nxbox.yml?branch=main&label=build"></a>
  <a href="https://github.com/NspxMiguel/NXbox/actions/workflows/port-tests.yml"><img alt="Port tests" src="https://img.shields.io/github/actions/workflow/status/NspxMiguel/NXbox/port-tests.yml?branch=main&label=port%20tests"></a>
</p>

<p align="center">
  <a href="#where-it-stands">Status</a> ·
  <a href="#what-to-expect">What to expect</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#trying-it">Trying it</a> ·
  <a href="#development">Development</a> ·
  <a href="#credits-and-lineage">Credits</a> ·
  <a href="docs/TRANSLATING.md">Translations</a> ·
  <a href="docs/MAINTENANCE.md">Maintenance map</a> ·
  <a href="docs/nxbox-port.md">Port notes</a>
</p>

<!-- Donation address: change it here and in "Support the project" only. -->
<p align="center">
  <a href="#support-the-project"><img alt="Support with Bitcoin" src="https://img.shields.io/badge/Bitcoin-support%20the%20project-F7931A?logo=bitcoin&logoColor=white"></a><br>
  <sub><code>bc1qm64el0gp0kvk7zqhl89x0vkngu2skqxd26vpjg</code></sub>
</p>

<table>
  <tr>
    <td><img src="docs/assets/screenshots/p5r-title.jpg" alt="Persona 5 Royal title screen on Xbox Series X"></td>
    <td><img src="docs/assets/screenshots/p5r-casino.jpg" alt="Persona 5 Royal casino gameplay on Xbox Series X"></td>
  </tr>
  <tr>
    <td><img src="docs/assets/screenshots/p5r-cutscene.jpg" alt="Persona 5 Royal animated cutscene on Xbox Series X"></td>
    <td>
      <b>Persona 5 Royal, on an Xbox Series X.</b><br><br>
      It boots, plays its cutscenes and reaches the first playable area (the casino), with
      audio. It still stutters on loads and has bugs: <b>no game is fully playable yet</b>.<br><br>
      Captured from the console with NXbox, using a copy of the game the owner bought. NXbox
      ships no games, firmware or keys.
    </td>
  </tr>
</table>

> **Experimental. No game is 100% playable without bugs yet.** Super Mario 3D World, Breath of the
> Wild and Persona 5 Royal reach gameplay on a console in Developer Mode, but running is not the same
> as working well: each has open bugs (see the [compatibility table](#compatibility)). There is no
> public release yet: builds come from CI.

> **\*New\* Native Direct3D 12 renderer.** The default path translates the Switch GPU to OpenGL and
> then, through Mesa, to Direct3D 12. The new renderer (`NXBOX_RENDERER=d3d12`) talks to Direct3D 12
> directly: Switch shaders are translated to HLSL and compiled with Microsoft's DXC, with no OpenGL or
> Mesa in between. It draws Super Mario 3D World's game-select screen correctly at 1920×1080 and
> 60 FPS, including text the OpenGL path drops, and reaches level 1-1, where lit geometry still
> renders black. Every one of the 4,511 shaders captured from four games translates to HLSL and
> compiles with DXC, checked offline by a corpus harness (`tools/nxbox/shader_corpus`). It is still
> opt-in. Plan and progress: [docs/d3d12-backend.md](docs/d3d12-backend.md).

## Where it stands

Everything below was measured on a retail Xbox Series X in Developer Mode.

| Area | State |
| --- | --- |
| CPU | Switch ARM64 code runs through the Dynarmic JIT inside the UWP sandbox, with a 128 MiB code cache |
| Graphics (default) | OpenGL 4.6 on Direct3D 12 through a patched Mesa `d3d12` driver, built in CI |
| Graphics (**\*New\***, opt-in) | Native Direct3D 12 renderer (`NXBOX_RENDERER=d3d12`): Switch shaders go Maxwell → IR → HLSL → DXC → DXIL. 60 FPS on Super Mario 3D World's menus and reaches level 1-1 (lit geometry still black). MSAA, async fences and recyclable buffer heaps are in; renderer errors are logged and the game keeps running, and a failed D3D12 start falls back to OpenGL. All 4,511 corpus shaders compile with DXC; tessellation is still limited |
| Frame pacing | OpenGL path: 60 FPS in Super Mario 3D World's hub, 30 FPS in Persona 5 Royal, about 12 FPS in Breath of the Wild, bound by the OpenGL/Mesa translation on the GPU thread (`NXBOX_THREAD_PROFILE=1` shows the ARM cores mostly idle). Loads still freeze the picture for a few seconds |
| Shaders | Per-title pipeline cache, precompiled at boot so scene changes do not compile shaders. A cache can be shared between consoles over the LAN |
| Memory | Guest memory is committed on demand. Persona 5 Royal peaks around 4.0 GB and Breath of the Wild around 4.5 GB of the 5 GiB app budget |
| Audio | XAudio2 output |
| Movies | Decoded on the CPU with FFmpeg |
| Textures | ASTC decoded on the CPU on the OpenGL path; the native D3D12 renderer decodes ASTC on the GPU |
| Input | Xbox controller, mapped to a Pro Controller |
| Accounts | A default offline profile with a Mii is created on first boot; firmware dumped from your own console can be installed from `LocalState\firmware` |
| Compatibility | No game is playable without bugs yet: see the [compatibility table](#compatibility) |

<details>
<summary><b>The hard parts so far</b>, each found on the console</summary>

- **Black screen for weeks**: Mesa's D3D12 driver began a pipeline-statistics query twice, so every
  command list failed to close and no GPU work ever ran. A known-color readback self-test exposed it.
- **Out of memory at 5 GiB**: the kernel zeroed guest memory with `memset`, committing the whole heap.
  On Xbox it now decommits pages instead.
- **Stutter at every scene change**: shaders compiled on first use. The pipeline cache is now
  precompiled on the renderer's own context, because a second GL context cannot be created on Mesa
  under UWP.
- **System starvation**: FFmpeg's default thread count starved the console's own services until the
  Device Portal stopped answering. It is now capped.

Written up in the [port notes](docs/nxbox-port.md).

</details>

## What to expect

- **There is no release date.** Nothing is promised until it is stable; releases will appear on the
  [releases page](https://github.com/NspxMiguel/NXbox/releases) when there is something to try.
- **No game is playable yet.** The table below is the honest state of each target.
- **Current focus: Super Mario 3D World, then The Legend of Zelda: Breath of the Wild**, and the new
  native Direct3D 12 renderer that both will move to. Mario Kart 8 Deluxe and Bayonetta come after.
- **Where it stands:** Super Mario 3D World reaches the 3D World hub at 60 FPS on the OpenGL path,
  with some views still drawing black polygons; the native Direct3D 12 renderer already draws its
  game-select screen correctly. Breath of the Wild loads a save (1.0, and 1.6 with both DLC) and runs
  in the Shrine of Resurrection at about 12 FPS; the GPU thread (OpenGL through Mesa) is the
  bottleneck, which the native renderer removes. Open bugs are
  tracked in the [issues](https://github.com/NspxMiguel/NXbox/issues).
- **How it is built:** by one person in their spare time, with AI coding tools doing much of the
  hard work on a small budget. Progress comes in bursts, and long waits are usually tool limits.
- **What you need:** an Xbox in Developer Mode, and your own game dumps, firmware and keys.
  NXbox ships none of them.
- **Asking for a game:** open an issue naming the title and version you own. Games the maintainer
  can buy and test move faster.

## Compatibility

Measured on a retail Xbox Series X in Developer Mode, with copies of the games the maintainer owns.
States: **Playable** (start to finish without graphical or stability problems), **In-game**
(gameplay runs, with bugs), **Menus** (reaches menus or a hub), **Boots**, **Not tested**.
Estimates are the maintainer's guess, not a promise; they move as bugs are found.

| Game | State | What happens today | Playable estimate |
| --- | --- | --- | --- |
| Super Mario 3D World + Bowser's Fury | **In-game** | 60 FPS on the OpenGL path: enters 3D World and reaches the hub with correct colours, but some camera angles draw black polygons and some text is missing. The native D3D12 renderer draws the menus correctly at 60 FPS and reaches level 1-1, with lit geometry still black | 2–4 weeks |
| The Legend of Zelda: Breath of the Wild (1.0, 1.6 + DLC) | **In-game** | Loads a save and runs in the Shrine of Resurrection at about 12 FPS with Link drawn correctly; some terrain is still broken. Bound by the OpenGL translation on the GPU thread, so it moves to the native renderer next | 1–3 months |
| Persona 5 Royal | **In-game** | Reaches the first playable area with audio; stutters on loads and has open bugs | 3–6 weeks |
| Mario Kart 8 Deluxe | **In-game** | With firmware dumped from the user's own console installed, it gets past the Mii picker and starts a Grand Prix race at up to 60 FPS on the OpenGL path; track geometry is still broken and fps drops during the race | 4–8 weeks |
| Bayonetta | Not tested | Planned after Breath of the Wild | — |

## How it works

```mermaid
flowchart LR
    game["Switch game (.nsp)"] --> core["Eden core<br/>HLE services · Dynarmic JIT"]
    core --> gl["OpenGL 4.6 renderer (default)"]
    gl --> mesa["Mesa d3d12 (patched)"]
    mesa --> d3d["Direct3D 12 on Xbox"]
    core --> native["*New* native D3D12 renderer<br/>Maxwell → HLSL → DXC"]
    native --> d3d
    core --> audio["XAudio2"]
    pad["Xbox controller"] --> core
```

- `src/eden_uwp/`: the Xbox frontend. It boots the game, owns the window and the controller, and
  writes `eden_uwp_diag.txt` with frame pacing, memory and stall attribution.
- `tools/nxbox/patch_mesa_uwp.py`: the Mesa patches, applied to a pinned
  [aerisarn/mesa-uwp](https://github.com/aerisarn/mesa-uwp) in CI.
- `src/video_core/renderer_d3d12/` and `src/shader_recompiler/backend/hlsl/`: the new native
  Direct3D 12 renderer and its HLSL shader backend.
- `tools/nxbox-pack/`: **\*New\*** a command-line tool for Windows, macOS and Linux that turns a game you
  own into its own Xbox app (no emulator menu) and installs it through the Device Portal.
- `tools/shader-share/`: a small server that keeps the largest valid shader cache per title.

## Trying it

You need an Xbox Series X|S in **Developer Mode** and your own dumps:

1. Install a package from the
   [package workflow](https://github.com/NspxMiguel/NXbox/actions/workflows/package-nxbox.yml)
   through the Device Portal.
2. Put `prod.keys` and `title.keys` in the app's `LocalState\eden\keys`.
3. Put your games (`.nsp`, `.nsz` or `.xci`) in `LocalState\games`, or on a USB drive in
   `NXbox\games` (the console's internal developer storage is small, a few GB). `.nsz` files are
   converted on the console the first time the library sees them.
4. Launch NXbox from the dashboard and pick the game in the library.

**\*New\*** [NXbox Pack](tools/nxbox-pack/README.md) does steps 1–4 for one game at a time: drag the
game, your keys and any updates/DLC into its window, and it installs that game as its own app that
boots straight into the game. To try the native renderer, add `NXBOX_RENDERER=d3d12` to
`LocalState\nxbox_env.txt`.

Two other ways to get games in are built but not yet tested on the console: **Add games** imports
games and keys from a USB drive (copy to the console, or move into `NXbox\games` to play from the
drive), and **Settings → Sources** browses and downloads from Tinfoil-format sources you add
yourself (none are included).

Details and the diagnostic switches are in the [port notes](docs/nxbox-port.md#booting-a-game-from-localstate-2026-09-25).
NXbox does not bundle games, firmware or decryption keys, and never will.

## Development

Read the [port notes](docs/nxbox-port.md) and the [UWP build setup](docs/uwp_build.md). The root
build requires CMake 3.31 or newer. The standalone memory-policy checks run without the emulator's
dependency graph:

```sh
cmake -S tests/port -B build-port-tests
cmake --build build-port-tests --config Release --parallel 2
ctest --test-dir build-port-tests -C Release --output-on-failure
```

## Support the project

<table>
  <tr>
    <td><img src="docs/assets/bitcoin-qr.svg" alt="QR code for the NXbox Bitcoin address" width="132"></td>
    <td>
      <img src="https://img.shields.io/badge/-%E2%82%BF-F7931A?logo=bitcoin&logoColor=white" alt="Bitcoin" height="20" align="top">
      <b>Bitcoin</b><br>
      <code>bc1qm64el0gp0kvk7zqhl89x0vkngu2skqxd26vpjg</code><br><br>
      Optional. It does not buy a release date or a specific game; it only helps pay for the tools
      that do the work.
    </td>
  </tr>
</table>

## Credits and lineage

**NXbox is a fork, not an emulator written from scratch.**

- [Eden](https://git.eden-emu.dev/eden-emu/eden) provides the emulator.
- [juanresendiz813/eden-xbox](https://github.com/juanresendiz813/eden-xbox) provided the starting
  Xbox/UWP work, including the imported boot frontend.
- [aerisarn/mesa-uwp](https://github.com/aerisarn/mesa-uwp) provides Mesa for UWP.
- Microsoft's [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (DXC)
  compiles the shaders of the native Direct3D 12 renderer.
- The **yuzu** and **Sudachi** projects and their contributors built the foundations Eden derives
  from.
- **Cheats** come from the community database
  [nx-cheats-db](https://github.com/sthetix/nx-cheats-db) by sthetix, reached the way
  [CNX Updater](https://github.com/CostelaCNX/CNX-Updater) by CostelaCNX (GPL-3.0) does. CNX Updater
  is a fork of [AIO-Switch-Updater](https://github.com/HamletDuFromage/aio-switch-updater) by
  HamletDuFromage (GPL-3.0), built on Borealis by natinusala, and their feature set inspired the
  downloads and cheats of NXbox. The data is fetched at runtime and belongs to its authors; see
  [NOTICE.md](NOTICE.md).
- Third-party libraries keep their own licenses and copyright notices.

Exact source revisions are recorded in the [provenance notes](docs/nxbox-port.md#source-provenance).
NXbox is an independent fork, developed with the help of AI coding tools, and its changes are not
sent upstream. It is not affiliated with or endorsed by its upstream projects,
Nintendo, Microsoft, Atlus or SEGA. The artwork was created for NXbox.

## License

GNU GPL version 3 or later; see [LICENSE.txt](LICENSE.txt). Existing source notices and third-party
licenses are kept.
