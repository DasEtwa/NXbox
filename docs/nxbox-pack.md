# NXbox Pack design

NXbox Pack creates an independent UWP app per base game using the existing NXbox game-mode runtime.
It copies the emulator binaries into each app rather than routing a launcher tile to a shared NXbox
installation. Each package gets a separate LocalState, saves, configuration and content. It does not
recompile Switch code into native Xbox code.

The implementation lives in `tools/nxbox-pack/`; `.github/workflows/nxbox-pack.yml` builds three
single executable targets using Bun 1.3.14. No emulation or graphics source is changed.

## Flow and runtime contracts

1. Read a local NSP/XCI/NSZ/XCZ, `prod.keys`, and optional add-ons. Detect Portuguese/English via
   locale; keep user-facing strings in one typed translation table. Never evaluate dragged input as
   shell code. Reject missing, empty or unsupported files.
2. Identify the base game using an unambiguous `[0100…]` filename token, or ask for its ID. IDs must
   be 16 hex digits, start with `0100` and end in `000`; update/DLC IDs cannot become app
   identities. Infer the display name from the filename and let the user correct it. This release
   does not decrypt the Control NCA or extract NACP/icon metadata. It announces that limitation.
3. Download a release or packaging CI artifact over HTTPS and verify its GitHub SHA-256 digest, or
   accept an explicit HTTPS URL plus a trusted SHA-256. CI downloads can use
   `NXBOX_PACK_GITHUB_TOKEN`, held in memory. Refuse an incomplete/CPU-only runtime. Extract only
   runtime EXE/DLL files, PNG assets and licensing notices; validate the x64 executable header.
4. Generate a manifest from the repository's `dist/nxbox/AppxManifest.xml`, embedded in the binary.
   Set identity `NSPX.NXbox.<TitleId>`, display name/description to the escaped game name, and
   retain executable `nxbox.exe`, entry point `NXbox.App`, VCLibs dependency, device family and
   capabilities. Remove the shared `nxbox://` protocol extension, avoiding multiple per-game owners
   of that scheme. Use the generic NXbox tile PNGs. Package version is initially `0.1.0.0`.
5. Decompress any NSZ/XCZ locally through optional `nsz`, with the supplied key path, in isolated
   temporary directories. Upload uncompressed content; neither the original game nor key file is
   copied into runtime staging.
6. Discover TCP port 11443 through the existing XboxDev host hint or `xbox.local`, then a bounded
   private-LAN /24 scan on active IPv4 interfaces (24 concurrent probes, at most 1024 candidates).
   Show candidates and ask for the address. There is no custom DNS-SD service enumeration;
   `xbox.local` uses the OS resolver, including mDNS where supported. A manually supplied address
   supports consoles outside the scanned /24 or custom ports.
7. Ask for credentials or opt into an existing secure record. Authenticate before offering to save.
   Request-local TLS bypass is explicit via `--insecure-portal`. Authenticate using Basic over
   HTTPS; obtain the CSRF cookie from a GET and echo it on POST. Never log credentials,
   authorization headers or response bodies. Password input closes readline before entering raw mode
   so the secret is not echoed by its terminal handler.
8. Upload the runtime into a uniquely named console loose-app folder, register it, poll deployment
   status, and locate the installed package by exact identity. Folder names include a UUID to avoid
   overwriting an existing registration's running runtime.
9. Create `LocalState/games` and `LocalState/eden/keys` through the file API; upload `prod.keys`,
   `base.nsp`/`base.xci` and `addon-N.nsp`/`addon-N.xci`. Stream multipart requests with known
   Content-Length and backpressure. Verify each file's reported remote byte size. Upload failures
   abort before creating direct-boot markers; game files are not hashed back from the console.
10. Write `LocalState/game.txt` containing `games\\base.nsp` (or `.xci`) and
    `LocalState/skip_library.txt` containing `1`. These markers are sent last. On subsequent tile
    activation, `RunGameView` bypasses `Ui::RunLibrary`; `ResolveGamePath` resolves the fixed path
    within that app's LocalState. `LocalState/eden/keys` matches `EDEN_DIR` / `KEYS_DIR` in
    `src/common/fs/path_util.cpp`. `RunGame` already enumerates `LocalState/games` as an external
    content directory, so updates and DLC can be read without an extra NAND copy.
11. Delete host temporary staging/decompressed content. Tell the user to set the new app's type to
    **Game** in Dev Home and launch its tile. The tool does not change global console settings.

## Cross-platform deployment choice

Use Xbox Device Portal's loose-folder upload and registration APIs. Microsoft's own wrapper uses:

- `POST /api/app/packagemanager/upload?destinationFolder=<Base64 UTF-8 folder>` with multipart
  files; the destination is relative to the console's loose-app area.
- `POST /api/app/packagemanager/register?folder=<Base64 UTF-8 folder>` followed by
  `GET /api/app/packagemanager/state` and `GET /api/app/packagemanager/packages`.
- `POST /api/filesystem/apps/folder` for directories and `POST /api/filesystem/apps/file` for data,
  with `knownfolderid=LocalAppData`, the returned `packagefullname`, rooted `/LocalState/...` paths
  and `extract=false`. File lists report `FileSize`; `SizeInBytes` is accepted for older wrappers.

The Xbox-specific upload/register endpoints are taken from
[Microsoft's Xbox deployment wrapper](https://github.com/microsoft/WindowsDevicePortalWrapper/blob/main/WindowsDevicePortalWrapper/WindowsDevicePortalWrapper.Shared/Xbox/XboxAppDeployment.cs).
The general
[Device Portal file API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core)
and
[Microsoft's file models](https://github.com/microsoft/WindowsDevicePortalWrapper/blob/main/WindowsDevicePortalWrapper/WindowsDevicePortalWrapper.Shared/Core/AppFileExplorer.cs)
provide the filesystem parameters and byte-size field. The existing
`~/.local/share/nxbox/console.py push` delegates to XboxDev's portal client; its measured rooted
LocalState paths are preserved here.

This route needs no host SMB server, Windows SDK, MakeAppx, SignTool or certificate generation. The
generic `networkapp` API instead refers to a network share; it is not interchangeable with Xbox's
local loose-folder `register` API. Registration still needs a Series-console test because the
published Xbox wrapper describes Xbox One.

Rewriting a signed APPX invalidates its signature. A generated per-game manifest therefore cannot
reuse the original APPX signature. A pure TypeScript MSIX packer plus self-signed certificate would
need block-map generation, correct APPX signing semantics and console certificate trust handling.
`osslsigncode` alone would not solve package layout and trust provisioning. Those paths are not
implemented because the Xbox loose registration API supplies the simpler SDK-free route. If the
console rejects loose registration, fail explicitly; do not install a guessed unsigned APPX.

## Credentials and ownership

macOS uses a dedicated Keychain generic-password service. Linux uses Secret Service (`secret-tool`)
when a desktop secret store is available. Windows first attempts PasswordVault; when unavailable, it
uses CurrentUser DPAPI and stores only ciphertext under the user's local data directory. Saving is
optional; if no secure backend works, credentials remain in process memory. No shared XboxDev record
is changed, and no Apple account, certificate or keychain configuration is altered.

Users must own each game and dump their own keys. The tool never obtains game dumps or keys from
network sources and never includes them in runtime staging or a distributable package. The only
network download is the emulator runtime. A configured app's private LocalState still contains
sensitive material: it must not be exported/shared. Source files remain on the user's computer;
compressed temporary output is removed after a handled success or failure. A process killed by the
OS may leave its private temporary directory behind.

## Verification and remaining questions

Host tests cover language precedence/overrides, supported extensions, dragged paths, filename IDs,
manifest identity/XML escaping/contracts, ZIP traversal and payload checks, SHA-256 mismatch,
authentication/CSRF, registration query encoding, streaming uploads and remote size verification. An
HTTP mock integration test stages a runtime, registers it, transfers synthetic base/add-on/keys, and
verifies exact direct-boot marker contents and ordering. It does not emulate Xbox deployment. The CI
matrix adds native compiled-binary smoke tests on Windows, macOS and Linux.

A real Xbox Series test must confirm:

- Nested loose-folder upload semantics, unsigned registration with `codeGeneration`, installed
  VCLibs resolution, and the deployment state response on the current Xbox OS.
- Two different games appear as independent named apps, each boots without any library screen, and
  saves/keys remain isolated. No automatic launch occurs before uploads finish.
- File API streaming works for dumps larger than 4 GiB, remote space exhaustion is surfaced, and the
  portal accepts rooted subfolders/returns the expected byte-size field.
- Optional NSZ/XCZ decompression, Unicode names, add-on loading and actual emulation compatibility.
- Windows PasswordVault/DPAPI, macOS Keychain and Linux Secret Service persistence and hidden
  password input on their native terminals. Tests use synthetic data and never access real keys.

Product questions remain: expose decrypted metadata through a small audited parser/helper; offer
per-title icons; publish public checksum-bearing game runtimes so ordinary users need no GitHub
credentials; ship Intel macOS/Linux ARM64 binaries; detect/set per-app Game classification; design
safe retry/update/cleanup and a monotonic package-version policy. Failed deployment may leave an
unregistered runtime directory; failed content transfer can leave an installed but unconfigured app.
These are reported failures, not successful game installs. Re-registering the same title currently
uses the same identity/version; preservation of LocalState and loose-folder lifecycle needs a
console test before offering an update workflow. There is no resume or uninstall/rollback command.
