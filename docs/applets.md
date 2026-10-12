# NXbox firmware applets — T17

Audit base: `ea3244c9cbfda4d2af18965fe4ddad586fd82267`. This describes implemented
loading routes, **not firmware compatibility certified on Xbox**. No firmware,
keys, game content, console credentials or Nintendo source is included.

## Selection and boot

UWP now tries the installed firmware for twelve local library applets, independent
of inherited desktop HLE defaults. Desktop settings retain their behavior.
Missing content, unsupported key generation or process initialization failure
falls back to the existing HLE frontend. Both library-applet creation IPC variants
use the same factory. The existing content provider and loader validate the program.

Settings > System menu > **Boot to Switch HOME menu** loads qlaunch from System
NAND as QLaunch/SystemApplet. It is optional, does not replace game.txt and does
not request game save synchronization. Missing/unloadable qlaunch uses the existing
launch failure screen. There is no HLE HOME substitute. A new selection requires
relaunching NXbox, as with the existing game session.

**View + Menu** sends one Switch HOME short press per chord. Individual View/Minus
and Menu/Plus mappings remain available; the chord suppresses both game buttons
while held. An individual button can precede the chord, so simultaneous pressing
is preferable. Xbox Guide is reserved by the Xbox shell and cannot be remapped by
UWP. Guide suspension pauses the whole Core::System, including guest applets;
resume requires releasing a held HOME chord before another press. An ordinary game
boot does not also launch qlaunch: a running HOME/overlay is needed to show that UI.

## Matrix

“LLE” means Eden has a program mapping and process-loading route, not that every
firmware service or visual interaction works. Base choices include UWP overrides.

| Applet | Program suffix | Eden LLE route | Base UWP choice | T17 UWP choice | HLE fallback |
| --- | --- | --- | --- | --- | --- |
| QLaunch / HOME | 1000 | Yes; desktop boot | No UI boot entry | Optional boot | None |
| Auth / parental controls | 1001 | Yes | LLE | Firmware-first | Parental controls |
| Cabinet / amiibo | 1002 | Yes | LLE | Firmware-first | Limited Cabinet frontend |
| Controller support | 1003 | Yes | Forced HLE | Firmware-first | Controller reconfiguration |
| DataErase | 1004 | Yes | HLE | HLE | Generic stub |
| Error viewer | 1005 | Yes | LLE | Firmware-first | Error frontend |
| NetConnect | 1006 | Yes | LLE | Firmware-first | Explicit cancellation; formerly never completed |
| ProfileSelect / player select | 1007 | Yes | Forced HLE | Firmware-first | Current emulated user |
| SoftwareKeyboard | 1008 | Yes | HLE | Firmware-first | Default text/inline frontend; no Xbox keyboard UI |
| MiiEdit | 1009 | Yes | Forced HLE | Firmware-first | Limited default Mii frontend |
| Web | 100A | Yes | HLE | Firmware-first | Limited host web shim |
| Shop / eShop | 100B | Yes | HLE | HLE; excluded | Existing web shim only |
| OverlayDisplay | 100C | Yes; manager boot | Optional enable_overlay | Unchanged | None |
| PhotoViewer / album | 100D | Yes | LLE | Firmware-first | Limited photo frontend |
| Settings | 100E | Yes | LLE if requested | Unchanged; no new boot entry | Generic stub |
| OfflineWeb | 100F | Yes | LLE | Firmware-first | Offline web shim |
| LoginShare | 1010 | Yes | HLE | HLE; excluded | Existing web shim only |
| WebAuth / captive portal | 1011 | Yes | HLE | Firmware-first | Captive-portal web shim |
| Starter / initial setup | 1012 | Yes | LLE if requested | Unchanged; no new boot entry | Generic stub |
| MyPage | 1013 | Yes | LLE | HLE; excluded | Generic stub |
| Lhub | No mapping | No | HLE-only | HLE-only | Lobby web shim |

Auth is parental controls; WebAuth is the captive portal. This track adds no
console-account authentication, dauth, BAAS, NPLN or eShop flow. Existing upstream
services are not evidence that firmware networking is usable.

## Recovery and lifecycle

A UWP guest library applet that terminates prematurely, before output or any
interactive exchange, can replay retained initial input through HLE using the same
caller broker. Normal ExitProcessAndReturn, cancellation, forced child termination,
shutdown or published output prevents replay. Recovery stays tracked until HLE
completes, including asynchronous callbacks. Input references are released on
output, interactive input or return; transfer storage is not serialized to disk.

There is no timer that mistakes an open dialog for failure. Running but hung guests,
failures after an interactive exchange, and qlaunch runtime crashes do not restart
through HLE. These require protocol-aware recovery and console evidence. NetConnect
HLE reports ResultLibraryAppletTerminated and signals completion, rather than
claiming a connection or waiting forever.

Existing WindowSystem recursion handles visible layers, HID ownership, caller/child
foreground and obscured focus, guest suspension and caller restoration. T17 reuses
that machinery. Whole-system UWP pause/resume applies to all guest applets.

## Measured coverage

| Measure | Base | T17 |
| --- | ---: | ---: |
| Firmware-first routes among the twelve local library applets | 6/12 (50%) | 12/12 (100%) |
| Optional UWP qlaunch boot entry | 0/1 | 1/1 |
| T17 tests | 0 | 5 unittest cases, including one compiled driver |

The portable driver exercises all 256 applet IDs, eight factory selection/fallback
combinations, 64 recovery flag combinations and HOME chord hold/release/resume.
Clang instrumentation for applet_policy.h and applet_boot.h measures 38/38 lines,
38/38 regions, 5/5 functions and 50/50 branch outcomes (100%). These numbers cover
**policy helpers**, not AM integration, guest firmware, WinRT or rendering. Baseline
helper code coverage is not applicable because these helpers did not exist.
Source-contract tests cover both creation IPC paths, recovery wiring, NetConnect
completion, qlaunch parameters, save-sync isolation and PT-BR/EN labels.

Acceptance commands:

```sh
cd tests/port
python3 -m unittest discover -s . -p 'test_*.py'
python3 -m unittest discover -s . -p 'test_t17_*.py'
gh workflow run build-nxbox.yml -R NspxMiguel/NXbox --ref team/t17-lle-applets
```

## Required Xbox Series X checks — owner's firmware 18.1.0

1. Boot HOME: record first-frame time, memory, qlaunch logs and navigation. Test
   missing/unloadable qlaunch with controlled emulated-NAND changes.
2. Use a generic homebrew applet harness for all twelve local applets: record guest
   program IDs, broker output, HID ownership and caller focus restoration. Repeat
   without firmware. Do not add title-specific exceptions.
3. Force early termination before output: verify one HLE recovery and completion.
   Normal return, RequestExit, Terminate, interactive exchange and shutdown must
   not replay. Check event/layer/process-holder counts and retained storage memory.
4. Suspend/resume with a foreground applet using Xbox Guide. Verify input returns to
   the applet, the caller stays obscured and a held HOME chord produces no new press.
5. Check HOME application/child transitions, layer composition and controller UI
   completion. The removed workaround reported firmware-controller hangs; removal
   alone does not establish that Controller LLE now exits correctly on Xbox.
6. Test normal/inline keyboard, Mii save/cancel, user select, album, offline web,
   error viewer and NetConnect cancellation. Default HLE frontends have limited UI.

## Integration boundaries

No src/video_core files, renderer defaults, accounts, certificates or firmware assets
are changed. If console evidence indicates missing layer composition, GPU ordering
or out-of-scope service calls, give the owning track the trace and a proposed patch.
The portable T17 driver should also be registered in the MSVC port-test runner by
its owner; tests/port/CMakeLists.txt is outside T17. The UWP workflow compiles the
actual T17 service/frontend changes with MSVC.
