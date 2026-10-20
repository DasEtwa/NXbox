# NOTICE

NXbox is a fork of [Eden](https://git.eden-emu.dev/eden-emu/eden) and is distributed under the GNU
GPL version 3 or later; see [LICENSE.txt](LICENSE.txt). Existing source notices and the licenses of
third-party libraries are kept as they are. This file adds the credits for the parts of NXbox that
were inspired by, or fetch data from, other projects.

## Attribution required for NXbox code (GPL-3.0 section 7(b))

Source files that carry the line `SPDX-FileCopyrightText: Copyright 2026 NSPX` were written for
NXbox. As an additional term permitted by section 7(b) of the GNU GPL version 3, anyone who conveys
those files, or a work that contains them, must:

1. keep that copyright line in every such file, and
2. keep the attribution "NXbox by NSPX (https://github.com/NspxMiguel/NXbox)" in the Appropriate
   Legal Notices of the work, such as its credits or about screen, and in its README or release
   notes.

Files that also carry another project's notice, such as Eden's, keep that notice too. This term
covers only the NSPX-copyrighted material; everything else stays under the plain GPL.

## Credits and third-party notices

### Cheats: nx-cheats-db, CNX Updater and the projects behind it

The **Cheats** screen of the mod store (Menu button) and the way it finds a game's cheats follow what
the Nintendo Switch homebrew updater CNX Updater offers. NXbox takes the same data from the same
place, and credits the people who built it.

- **nx-cheats-db** by **sthetix** (<https://github.com/sthetix/nx-cheats-db>) is the cheats
  database. NXbox does not ship it. It downloads `titles.zip` (with `contents.zip` as a fallback)
  from that project's latest GitHub release when the player opens the Cheats screen of a game, keeps
  a copy for 24 hours in the app's local storage, and writes only the cheats the player turns on
  into the game's `NXboxCheats` mod folder. The cheats in the database belong to their authors and
  to the nx-cheats-db project; NXbox claims no rights over them.
- **CNX Updater** by **CostelaCNX** (<https://github.com/CostelaCNX/CNX-Updater>, GPL-3.0) is the
  updater whose feed points at nx-cheats-db (`cheats_config` with `url_titles` and `url_contents`).
  NXbox uses the same two addresses. No CNX Updater source code is copied; the feature set and the
  data source are the inspiration.
- **AIO-Switch-Updater** by **HamletDuFromage** (<https://github.com/HamletDuFromage/aio-switch-updater>,
  GPL-3.0) is the project CNX Updater is a fork of.
- **Borealis** by **natinusala** (<https://github.com/natinusala/borealis>) is the UI library those
  updaters are built on. NXbox does not use it; it is credited because the updaters' feature set,
  which inspired the downloads and cheats of NXbox, comes from that line of work.

NXbox is an independent project. It is not affiliated with or endorsed by any of the projects above.

The credit line "Cheats database: nx-cheats-db by sthetix, via CNX Updater by CostelaCNX (GPL-3.0)"
is shown on the Cheats screen, and Settings > Credits lists the same names.

### Emulator and platform

- **Eden** (<https://git.eden-emu.dev/eden-emu/eden>), the emulator NXbox is forked from, and the
  **yuzu** and **Sudachi** projects it derives from.
- **juanresendiz813/eden-xbox** (<https://github.com/juanresendiz813/eden-xbox>), which provided the
  starting Xbox/UWP work.
- **aerisarn/mesa-uwp** (<https://github.com/aerisarn/mesa-uwp>), which provides Mesa for UWP.

NXbox ships no games, firmware or keys.

### AMD FidelityFX Super Resolution 1

The D3D12 presentation upscaler uses the vendored FidelityFX FSR 1 EASU and RCAS
implementation in `externals/FidelityFX-FSR/ffx-fsr`, compiled as HLSL with DXC.

Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
