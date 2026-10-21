# Experimental Switch homebrew (.nro) support

NXBox already contains Eden's `AppLoader_NRO`. This branch exposes basic NRO discovery
in the Xbox library and USB import paths. **It is an experimental boot path, not a
claim that individual homebrew apps run correctly.**

The [2026-10-21 compatibility audit](pipensx-compatibility-audit.md) records the
three review fixes, executable regressions, the verified PipenSX 1.6.4 NRO
metadata, and concrete gaps in the guest installation services. Host tests do
not establish Xbox runtime compatibility.

## First smoke test: PipenSX

Source: https://github.com/i3sey/pipensx (GPL-3.0).

1. Download `pipensx.nro` from the project's official release. No ROMs, catalogs,
   keys or firmware are included in this test.
2. In the Xbox Device Portal, open **File explorer > LocalAppData > the NXBox
   package > LocalState > games**. Create `games` if it is missing and upload
   `pipensx.nro`. Alternatively place it in an external `NXbox/games` folder.
3. Install a development Appx built from **this branch**, launch NXBox, rescan
   the library using **Add games**, and select `pipensx`.
4. Check whether it reaches its UI. If it crashes or returns to NXBox, capture
   `LocalState/eden_uwp_diag.txt` and `LocalState/nxbox_crash.txt` from Device Portal.
   Record the first missing-service, filesystem, graphics or networking error.
5. Repeat on a clean console session and log Xbox model, NXBox commit, PipenSX
   release, and whether the error is reproducible.

### Pre-existing direct-boot diagnostic (no frontend patch required)

On an existing NXBox install, an NRO can also be probed via the core's direct
boot path. Put `pipensx.nro` in `LocalState/games`, create `LocalState/game.txt`
containing exactly `games\\pipensx.nro`, and create an empty
`LocalState/skip_library.txt`. **Back up a pre-existing game.txt before changing
it** and remove `skip_library.txt` after the test. This bypasses the library;
it does not bypass unimplemented guest APIs.

## Known limitations

- PipenSX uses libnx, Borealis, sockets, SD-card paths, background network
  transfers and native Switch install services. Those must work in Eden's HLE
  before PipenSX can function end-to-end.
- The NRO loader **does not make an Xbox UWP app out of PipenSX**. PipenSX runs
  as emulated Switch ARM64 code inside NXBox.
- Installing an NSP inside an emulated Switch is separate from installing an
  Xbox Appx. NXBox does not install Switch content to the Xbox dashboard.
- NXBox may lack compatible SD-card mounts, guest TCP/IP behavior, libnx service
  commands, writable title databases, or install APIs. Do not assume that
  catalog browsing, torrent transfers or NSP installation works.
- NRO library entries use a path-derived, synthetic 64-bit identifier to avoid
  deduplicating unrelated homebrew without NACP/title IDs. This identity is
  only for the NXBox UI and is not a Nintendo program/title ID.
- USB import and library presentation are not yet exercised by Windows UWP CI,
  and no Series S runtime verification has been completed for this branch.
- This branch neither includes nor provides Nintendo games, keys, firmware,
  or an unauthorized content catalog. Test only with content you may lawfully use.

## Definition of done for PipenSX compatibility

- [ ] NRO appears in the library and launches on Xbox Series S.
- [ ] Borealis UI initializes with controller input and renders.
- [ ] Homebrew SD-card paths persist across app restarts in the emulated filesystem.
- [ ] Guest network initialization and a benign HTTPS request work.
- [ ] A legal test download finishes and survives restart.
- [ ] All installation-related guest service calls are audited and either
      implemented with bounded, virtualized writes or clearly unsupported.
- [ ] Failures show actionable user-facing diagnostics instead of crashing.

Treat the above as a staged compatibility project, not a one-line file-extension fix.
