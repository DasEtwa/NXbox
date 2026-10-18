# NxbHelper design

> **Naming:** NxbHelper was called NXbox Pack. The product name changed, but the folder
> `tools/nxbox-pack/`, the `nxbox-pack` workflow and the `NXBOX_PACK_*` variables keep the old name,
> so paths and identifiers below still say `nxbox-pack`.

NxbHelper creates an independent UWP app per base game using the existing NXbox game-mode runtime.
It copies the emulator binaries into each app rather than routing a launcher tile to a shared NXbox
installation. Each package gets a separate LocalState, saves, configuration and content. It does not
recompile Switch code into native Xbox code.

The implementation lives in `tools/nxbox-pack/`; `.github/workflows/nxbox-pack.yml` builds three
single executable targets using Bun 1.3.14. No emulation or graphics source is changed. Windows CI
embeds its installed native Bun executable instead of downloading the baseline cross-target ZIP (Bun
1.3.14 fails to extract that ZIP on Windows). Cross-compilation from other hosts retains the Windows
x64 baseline target. The native Windows artifact inherits the installed Bun runtime's CPU
requirements.

## Flow and runtime contracts

1. Read a local NSP/XCI/NSZ/XCZ, `prod.keys`, optional add-ons, firmware, save data, mods and
   `title.keys`. Detect Portuguese/English via locale; keep user-facing strings in one typed
   translation table. Never evaluate dragged input as shell code. Reject missing, empty or
   unsupported files.
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
   game files under `Base/`, `Updates/` or `DLC/` according to content metadata or title ID,
   retaining their original names (unknown files stay in `games/`). Stream multipart requests with known
   Content-Length and backpressure. Verify each file's reported remote byte size. Upload failures
   abort before creating direct-boot markers; game files are not hashed back from the console.
10. Transfer optional firmware into a flat `LocalState/firmware/`, saves into Eden's NAND user save
    tree, mods into `LocalState/eden/load/<titleid>/<modname>/`, and `title.keys` into
    `LocalState/eden/keys/`. Create every destination folder with the Portal folder API before
    uploading; verify remote byte sizes just like game content.
11. Write `LocalState/game.txt` containing the relative `games\\Base\\<original name>.nsp` path (or `.xci`, with unknown content at the root) and
    `LocalState/skip_library.txt` containing `1`. These markers are sent last. On subsequent tile
    activation, `RunGameView` bypasses `Ui::RunLibrary`; `ResolveGamePath` resolves the fixed path
    within that app's LocalState. `LocalState/eden/keys` matches `EDEN_DIR` / `KEYS_DIR` in
    `src/common/fs/path_util.cpp`. `RunGame` already enumerates `LocalState/games` as an external
    content directory, so updates and DLC can be read without an extra NAND copy.
12. Delete host temporary staging/decompressed content. Tell the user to set the new app's type to
    **Game** in Dev Home and launch its tile. The tool does not change global console settings.

## Optional content and running checklist

After updates/DLC, the CLI asks for firmware, a save backup, mods (repeatable), and `title.keys`.
Enter skips an optional step. Every accepted input updates the same running checklist; rejected
optional inputs explain the problem in the selected system language and allow another drag-and-drop.
Portuguese and English use the existing typed translation registry, with per-key English fallback.
Firmware is recommended: without it, Mii screens and some system applets stay empty.

Firmware inputs can be an NCA folder, an NCA ZIP, `rawnand.bin`, a contiguous set of
`rawnand.bin.00` through `.NN`, or a ZIP containing either NAND layout. Dropping any split part
selects the contiguous sibling set. A ZIP with multiple/ambiguous NAND sets is rejected. The CLI
reads GPT and decrypts only SYSTEM with the 32-byte `bis_key_02` crypt/tweak halves from the
supplied `prod.keys`. Random reads use 0x4000-byte AES-XTS sectors; no decrypted full NAND is
written. For compressed ZIP members, a streaming pass discards all other partitions and spools only
encrypted SYSTEM into the private temporary directory. ZIP64 is supported; split-volume ZIPs are
not.

The minimal FAT32 reader walks `/Contents/registered`, including hashed subdirectories and long
filenames. An NCA directory's contiguous `00`, `01`, ... files are concatenated in order. Each
result must have a 32-hex NCA name equal to the first 128 bits of its SHA-256, an encrypted NCA3
header decryptable with `header_key`, and a matching declared file size. The set must contain
`0100000000000809` Data (SystemVersion) and `0100000000000802` Data/PublicData (Mii model).
SystemVersion is read from the RomFS `file` structure, including AES-CTR sections using the matching
`key_area_key_<family>_<generation>`. Missing keys and incomplete sets have separate messages. The
runtime performs its existing full archive integrity verification and installation at boot; the
CLI's checks do not replace that verification.

Bun 1.3.14 does not expose `aes-128-xts` on every platform. The crypto adapter uses that
`node:crypto` cipher when available, otherwise implements block-aligned XTS using native
`node:crypto` AES-ECB primitives. Tests check a fixed independent nonzero little-endian sector
vector and a complete 0x4000-byte sector digest. Nintendo NCA headers use big-endian sector tweaks.
SYSTEM tries the requested little-endian convention first, then Nintendo's big-endian convention (as
used by Eden's `AesXtsStorage`); a full validated NCA set determines the convention, not sector
zero.

For saves, drop **one extracted JKSV/Checkpoint backup leaf** for this game, not the entire backup
manager directory. A ZIP may contain the leaf's files directly or one enclosing backup directory.
The latter wrapper is stripped once; folder inputs preserve all subdirectories. Explicit title IDs
in backup paths must match the selected game. Unlabelled leaf backups cannot be identified from
payload alone: the user selects the backup for the current title. The CLI prepares one Player
profile in Eden's binary `profiles.dat` format and copies files to
`LocalState/eden/nand/user/save/0000000000000000/<profile-u128>/<titleid>/`. It refuses save import
if the installed app already has `profiles.dat`; existing profile/save migration is not implemented.
Device-wide saves and encrypted raw Switch saves are not JKSV/Checkpoint backup leaf imports.

Mods accept a mod folder/ZIP containing `romfs` and/or `exefs`, an Eden `load/<titleid>/<modname>/`
tree, or a direct `romfs`/`exefs` folder. Direct folders share the `Imported` mod name; other
layouts retain their mod names. Repeat the mods prompt to add another folder/ZIP; duplicate
destination files and mismatching title IDs are rejected. Files go into
`LocalState/eden/load/<titleid>/<modname>/romfs|exefs/...`. `title.keys` is optional and must be a
nonempty file with that name. It is transferred to private LocalState, never runtime staging.

ZIP extraction streams each member and validates CRC/size on complete members. Unsafe paths,
symlinks, case collisions, encrypted ZIPs and unsupported compression methods are rejected. During
NAND ZIP slicing, skipped bytes outside SYSTEM are not checked against a member's full CRC;
extracted NCAs are individually hashed instead. Temporary SYSTEM/NCAs/saves/mods are private and
removed on handled success or failure. An OS kill may leave the private temporary directory behind.
Firmware, saves, mods and keys are never included in a compiled binary, runtime package or
repository.

Example of the accepted-input checklist in pt-BR (illustrative file counts):

```text
  [x] Jogo 01007EF00011E000 (13.4 GiB)
  [x] Chaves (prod.keys)
  [x] Update 01007EF00011E800 (3.0 GiB)
  [ ] DLC -
  [x] Firmware 18.1.0 — 229 arquivos
  [x] Dados salvos 3 arquivos
  [x] Mods 2 arquivos
  [x] Chaves (title.keys) title.keys
```

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
verifies exact direct-boot marker contents and ordering. It does not emulate Xbox deployment.
Synthetic tests also build encrypted FAT32/GPT images with chunked NCAs, exercise raw/split/ZIP
NAND, encrypted SystemVersion, independent XTS vectors, save profiles, mods, ZIP corruption and
optional transfer folder/marker ordering. No real keys, firmware or game data are used. The CI
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

Organize se quiser: games\Base (jogo), games\Updates (atualizações), games\DLC (DLCs). Tudo solto em games\ também funciona.

Optionally organize: games\Base (game), games\Updates (updates), games\DLC (DLC). Everything loose in games\ works too.
