# Desktop D3D12 testing on Linux

This build runs NXbox's native `src/video_core/renderer_d3d12` renderer in a Windows x64 Win32
process. Wine/Proton supplies Windows APIs; vkd3d-proton translates its D3D12 commands to Vulkan.
Selecting `-r d3d12` does not select Eden's Vulkan renderer.

## Build and artifact

Run the **Build NXbox desktop D3D12** GitHub Actions workflow manually, or use the automatic build
on `main`. Download artifact `nxbox-desktop-d3d12`, then extract the zip inside it. Keep
`eden-cli.exe`, every DLL and the license files together. The package includes the same
checksum-pinned DXC distribution used by Xbox packaging: `dxcompiler.dll`, `dxil.dll` and the
app-local MSVC runtime. HLSL is compiled at runtime.

For a local Windows build, install Visual Studio with the x64 C++ tools and Windows SDK, CMake
3.31+, Ninja, Python, NASM and Perl. From the repository root run:

```bat
python tools\nxbox\prepare_host_tools.py
tools\nxbox\build-desktop-d3d12.cmd
set NXBOX_DXIL_SDK=0
python tools\nxbox\prepare_dxil.py --output build-desktop-d3d12/bin
cmake -DNXBOX_BUILD_DIR=build-desktop-d3d12 -P tools/nxbox/package_desktop_d3d12.cmake
```

The target is named `yuzu-cmd`; its executable is `eden-cli.exe`. Qt is disabled. D3D12 can be
disabled on desktop with `-DENABLE_D3D12=OFF`. Xbox UWP always retains it. OpenGL remains the
default renderer. Desktop textures use the adapter's reported memory and the standard cache
watermarks; Xbox's fixed watermarks remain UWP-only.

## Wine with vkd3d-proton

Use an x86-64 Linux host and a working NVIDIA Vulkan driver for the RTX 3050. Check
`vulkaninfo --summary`. Use current Wine, vkd3d-proton and DXVK releases compatible with that
driver. vkd3d-proton requires Vulkan 1.3 and obtains DXGI from DXVK; see its
[official driver and installation notes](https://github.com/HansKristian-Work/vkd3d-proton#drivers).

Create a dedicated prefix and unpack the NXbox package under its drive C:

```sh
export WINEPREFIX="$HOME/.local/share/wineprefixes/nxbox-d3d12"
export WINEARCH=win64
wineboot -u
mkdir -p "$WINEPREFIX/drive_c/nxbox"
unzip /path/to/nxbox-desktop-d3d12.zip -d "$WINEPREFIX/drive_c/nxbox"
```

Unpack a vkd3d-proton release and run its `setup_vkd3d_proton.sh install` with this `WINEPREFIX`
exported. Install the matching DXVK **x64** `dxgi.dll` into `$WINEPREFIX/drive_c/windows/system32`.
Use native overrides for the translation DLLs; leave Microsoft's `dxcompiler.dll` and `dxil.dll`
beside Eden, as shipped:

```sh
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n;dxcompiler,dxil=n"
export NXBOX_D3D12_FRAME_PROFILE=1
export NXBOX_THREAD_PROFILE=1
export VKD3D_DEBUG=info
export VKD3D_FILTER_DEVICE_NAME="RTX 3050"
cd "$WINEPREFIX/drive_c/nxbox"
wine ./eden-cli.exe -r d3d12 -g 'Z:\home\miguel\Games\game.nsp'
```

Replace the game path with your own dumped `.nsp`, `.xci` or supported executable. Wine normally
maps Linux `/` to `Z:`; `winepath -w /absolute/path/to/game.nsp` also produces the Windows path.
`--renderer d3d12` is equivalent. In the SDL configuration, `[Renderer]` / `backend=5` selects the
same enum value; `-r` overrides the config and `-n` overrides it with Null. Other CLI renderer names
are `opengl`, `vulkan` and `null`. The desktop D3D12 window uses SDL3's native HWND and
`CreateSwapChainForHwnd`. An initialization failure is reported instead of silently running another
backend.

## Proton alternative

Add `eden-cli.exe` as a non-Steam game in Steam, choose a Proton compatibility tool, set its start
directory to the extracted package and its launch arguments to `-r d3d12 -g "Z:\path\to\game.nsp"`.
Set launch options to:

```text
NXBOX_D3D12_FRAME_PROFILE=1 NXBOX_THREAD_PROFILE=1 VKD3D_DEBUG=info %command% -r d3d12 -g "Z:\path\to\game.nsp"
```

Proton already provides vkd3d-proton and DXVK/DXGI. Use Steam's Proton prefix for that shortcut,
rather than the separate Wine prefix above. Its user directory is below
`steamapps/compatdata/<shortcut-id>/pfx/drive_c/users/steamuser/AppData/Roaming/eden`. Proton's
supported launch behavior is documented in the
[Proton repository](https://github.com/ValveSoftware/Proton).

## Keys, firmware and logs

The Windows user directory defaults to `%APPDATA%\eden`, typically:

```text
$WINEPREFIX/drive_c/users/<wine-user>/AppData/Roaming/eden/
  keys/prod.keys
  keys/title.keys
  nand/system/Contents/registered/    (installed firmware content)
  config/sdl2-config.ini
  log/eden_log.txt
```

`wine cmd /c echo %APPDATA%` shows the actual prefix path. Use your own console's keys and firmware.
The CLI has no firmware installer: copy an already installed Eden firmware content directory into
`nand/system/Contents/registered`. Creating a directory named `user` beside `eden-cli.exe` enables
portable mode and overrides `%APPDATA%\eden`; keys, firmware, config and logs then live there. This
also works with Proton.

## Diagnostics and capture

| Variable                            | Effect                                                                                                                                                                    |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `NXBOX_D3D12_DEBUG=1`               | Requests the D3D12 debug layer and logs InfoQueue messages when available. Wine may not provide the Microsoft debug layer; failure is logged and execution continues.     |
| `NXBOX_D3D12_FRAME_PROFILE=1`       | Logs `D3D12_FRAME_PROFILE`: frame counts, CPU phases, draw/dispatch/PSO/submit counts and fence wait time.                                                                |
| `NXBOX_THREAD_PROFILE=1`            | Every five seconds logs per-thread `GetThreadTimes` CPU percentages, thread names, sampling coverage, and host present/fence wait totals. The first sample is a baseline. |
| `VKD3D_DEBUG=info`                  | Enables translation-layer diagnostics, independently of Eden's log.                                                                                                       |
| `VKD3D_FILTER_DEVICE_NAME=RTX 3050` | Filters the Vulkan device by name.                                                                                                                                        |

The existing `DRAW_SKIP` diagnostics remain in the normal Eden log. Use a log filter such as
`*:Info` to retain thread samples (in `[Miscellaneous]`, `log_filter=*:Info`). Search `eden_log.txt`
for `D3D12 initialized`, `pipeline path=Maxwell/HLSL/DXIL`, `DRAW_SKIP`, `D3D12_FRAME_PROFILE` and
`THREAD_PROFILE`. No Xbox LocalState or `nxbox_crash.txt` is required on desktop.

For RenderDoc, launch the Linux Wine process from RenderDoc with the same environment and arguments,
enable child-process capture, and capture the translated **Vulkan** frame. The Linux capture is
downstream of vkd3d-proton; Windows RenderDoc can capture D3D12 without Wine. A vkd3d-proton
development build with `-Denable_renderdoc=true` also offers shader-triggered captures; consult the
[official advanced debugging notes](https://github.com/HansKristian-Work/vkd3d-proton#advanced-shader-debugging).
Keep captures and game data outside the repository.

## Verification boundary

Source-level port tests cover desktop/UWP source selection, CLI routing, SDL HWND plumbing and
desktop DXC/profiler wiring. A successful Windows CI compilation and a real Wine/Proton game run are
separate checks. This work does not add missing renderer features: existing
screenshot/applet-capture placeholders and unsupported guest-work fallbacks remain.

Organize se quiser: games\Base (jogo), games\Updates (atualizações), games\DLC (DLCs). Tudo solto em games\ também funciona.

Optionally organize: games\Base (game), games\Updates (updates), games\DLC (DLC). Everything loose in games\ works too.
