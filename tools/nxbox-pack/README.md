# NxHelper

Prepare a USB drive or send locally owned games to an installed NXbox through Device Portal. You can
also install a separate NXbox UWP app for each game. The app has its own identity, name, tile and
private saves/keys/content. Once configured, its tile boots directly into the game, without opening
the NXbox library. This packages an emulator runtime; it does not recompile the game.

Download a binary from the **Build NxHelper** workflow artifacts:

| File                       | Host                 |
| -------------------------- | -------------------- |
| `nxhelper-windows-x64.exe` | Windows x64          |
| `nxhelper-macos-arm64`     | Apple Silicon macOS  |
| `nxhelper-linux-x64`       | Linux x64 with glibc |

On macOS/Linux, run `chmod +x nxhelper-*` before launching. Run the binary in a terminal. No Bun,
Windows SDK, MakeAppx, SignTool, OpenSSL, signing certificate or emulator build is required for
uncompressed NSP/XCI files. An Intel Mac binary can be compiled with `bun-darwin-x64`; CI currently
ships the three targets above.

## Use

Enable Xbox Developer Mode and Device Portal authentication in **Dev Home > Remote Access**. The
console must already have `Microsoft.VCLibs.140.00` (x64, version 14.0.30704.0 or newer).

```sh
./nxhelper-macos-arm64 --insecure-portal
```

The self-signed Device Portal TLS certificate normally requires `--insecure-portal`. This flag
applies only to the selected portal connection. Omit it if the console certificate is trusted. Use a
trusted local network when accepting an unverified certificate.

For **Install a game as its own app**:

1. Drag one game file and press Enter.
2. Drag `prod.keys` and press Enter.
3. Optionally drag updates/DLC, one per prompt, then press Enter without a file.
4. Confirm the filename-derived game name and ID. If there is no unambiguous bracketed base ID such
   as `[01007EF00011E000]`, enter it. Control NCA metadata extraction is not implemented; the tool
   explicitly announces the fallback and uses the generic NXbox tile images.
5. Confirm the discovered Xbox address or enter the address shown in Dev Home.
6. Enter Device Portal credentials; the password is hidden. You can opt into the OS secure store.
7. After installation/upload completes, set this game's app type to **Game** in Dev Home and open
   its tile. Registration and boot still need validation on an actual Xbox Series console.

Only Portuguese and English are used, selected automatically from the system locale. `LC_ALL`,
`LC_MESSAGES` and `LANG` take precedence over `Intl`; on Windows, `Get-Culture` supplies the locale
when no environment override is present. `NXHELPER_LANG=pt` or `en` forces the language for tests.
There is no language selector.

Credentials stay in memory unless you choose to save them. macOS uses Keychain via `security`; Linux
uses Secret Service via `secret-tool`; Windows tries PasswordVault and falls back to a CurrentUser
DPAPI-encrypted record. If secure storage fails, the session continues without saving. Passwords are
never passed in process arguments or written as plaintext configuration.

## Runtime source

The default source is the latest `NspxMiguel/NXbox` release's `NXbox_game_*.appx` with a GitHub
SHA-256 digest. If unavailable, the tool tries the newest successful main-branch packaging workflow
runs and their unexpired `nxbox-release` / `nxbox-diagnostic` artifacts. CPU-only artifacts fail
payload validation. For authenticated CI downloads, set `NXHELPER_GITHUB_TOKEN` to a token with
Actions read access for this repository. The token is not saved. A public release is the preferred
distribution path for users without GitHub credentials.

Alternatively, supply both a direct HTTPS URL and a trusted checksum:

```sh
./nxhelper-macos-arm64 --insecure-portal \
  --runtime-url https://example.org/NXbox_game_0.3.1.0_x64.appx \
  --runtime-sha256 YOUR_64_HEX_DIGIT_SHA256
```

A URL may point to a game APPX, a packaging CI artifact ZIP containing that APPX, or a ZIP of the
staged runtime (`nxbox.exe`, required DLLs, `Assets/`, `Notices/`). Do not use an unprepared raw
`nxbox-uwp-build` artifact: it lacks the complete game payload. ZIPs are handled in TypeScript; no
external unzip command is needed. Downloads are limited to 256 MiB compressed and ZIP contents to
768 MiB expanded per archive. SHA-256 is checked before extraction. Runtime staging retains licenses
and strips signed APPX bookkeeping, homebrew samples, game dumps, keys and stale boot configuration.

## Compressed games

For independent app installation and full export, NSZ/XCZ files are accepted but need the optional
[nsz decompressor](https://github.com/nicoboss/nsz) on PATH. NxHelper runs
`nsz -D --keys <your-prod.keys> -o <private-temp-directory> <dump>` locally before deployment. It
uploads the resulting NSP/XCI and deletes temporary output afterward. The original dump is
preserved. Allow space for both the compressed input and decompressed output; updates/DLC follow the
same process. Without `nsz`, use an NSP/XCI dump. The CLI reports the missing requirement instead of
uploading compressed files that cannot boot through `skip_library.txt`.

## Big games

The Device Portal file API fails on a single multi-GB upload (measured at ~3.6 GB), so any file
above 1 GiB is sent in 1 GiB parts (`base.nsp.nxpart000`, `.nxpart001`, ... then a
`base.nsp.nxparts` manifest, last). NXbox joins the parts into the original file on its next start;
leave the console with enough free space for one more part while it does.

## Development

Use Bun **1.3.14** (`d1632b29`; local full revision `d1632b291`):

```sh
cd tools/nxbox-pack
bun install --frozen-lockfile
bun run check
bun test
bun run build:windows
bun run build:macos
bun run build:linux
```

Every build command includes `src/cli.ts`, fixing the "Missing entrypoints" error from invoking
`bun build` alone. The CI matrix tests and builds on all three OSes, runs each compiled executable
with English/Portuguese `--help`, and uploads the binary with its SHA-256 sidecar. Generated
binaries and dependencies are ignored by Git.

See [the design and console verification plan](../../docs/nxbox-pack.md).

## Ownership

You must own the game and dump your own keys. NxHelper never downloads games or keys. They are
uploaded separately into the installed app's private LocalState, never included in the staged
runtime or a distributable package. Do not share game dumps, keys, or console LocalState backups.

Profile characters share NXbox's 120 original portraits and 40 named backgrounds, including
`sunrise`, `lagoon`, `dusk`, and `ember` gradients. The stable `fox-pilot` ID now represents a fox
ceramic artisan. The console's Settings > Account > Profile picture works in single-account mode;
Multiple accounts is off by default and no first-launch account question appears.

## Send games to an installed NXbox

The first interactive question offers **With a USB drive (recommended)**, **Over Device Portal**,
and **Install a game as its own app**. The GUI offers these choices plus full library installation
and **Check USB drive**. Updates and DLC can be selected alongside base games.

```sh
nxhelper --to-drive /Volumes/Xbox Game.NSP Update.nsp --keys /path/prod.keys \
  --title-keys /path/title.keys --firmware /path/firmware.zip
nxhelper --to-xbox --host 192.168.1.20 Game.nsp --keys /path/prod.keys
nxhelper --check-drive /Volumes/Xbox
nxhelper --check-drive /Volumes/Xbox --fix
```

`--keys` is required for the transfer commands. Title keys and firmware are optional. Firmware may
be a folder of loose NCAs (including nested `Contents/registered` trees and `.cnmt.nca` metadata),
or NCA chunk directories named `<hash>.nca` with consecutive `00`, `01`, … files. ZIP archives
support these layouts, DEFLATE and ZIP64. NAND sources include `rawnand.bin`, Hekate numbered
splits, and file-based emuMMC (`emuMMC/<name>/eMMC/00`, `01`, …; boot0/boot1 are ignored). Select
the drive root, `emuMMC`, or `eMMC` folder. Standalone `SYSTEM` and `SYSTEM.bin` images are
accepted, either already decrypted or encrypted with `bis_key_02`. Encrypted NAND/SYSTEM images
require the matching `bis_key_02`; decrypted images do not. Extract `.7z`/`.rar` archives first. For
partition-based emuMMC, use Hekate to make a file backup. Keys and complete NSP/NSZ containers are
validated before transfer; firmware is verified by the existing NCA verifier. Convert XCZ to XCI
before USB preparation. Device Portal transfers can decompress XCZ with the optional `nsz` tool.
NSP, NSZ and XCI extensions are lowercased automatically.

USB preparation writes keys at the drive root, games in `NXbox/games/`, and verified firmware NCAs
loose in `NXbox/firmware/`. It checks free space and destination conflicts before copying. Existing
files with the same size are skipped; different sizes stop the operation without overwriting. If
older keys exist in `switch/`, move them aside first: Xbox reads those before the root keys. The
final checklist may still require firmware if none was supplied or already present.

Device Portal transfer finds the newest installed `NSPX.NXbox_` library package. It sends games to
`LocalState/games`, keys to `LocalState/eden/keys`, then firmware to `LocalState/firmware`. Open
NXbox afterward to import firmware. Credentials and discovery use the same paths as app
installation. Capacity is checked when the portal exposes it; otherwise a warning recommends USB
because the internal SSD fills quickly. Multi-GB files use the existing split uploader.

The drive checklist follows Xbox's exact lookup locations and detects missing/misplaced keys,
unreadable firmware layouts, incomplete NSP/NSZ files, unsupported extensions, missing filename IDs,
and misplaced games. It exits 0 when there are no failed items, 1 otherwise. `--fix` only unpacks or
flattens firmware into `NXbox/firmware/`; it preserves the original ZIP, NAND or directory.

For the native GUI, build with `cargo build --release --locked` in `tools/nxbox-pack-gui/`. Place
the compiled core alongside `NxHelper(.exe)` as `nxhelper-core(.exe)`. The JSON-lines API accepts
`checkDrive`/`fixDrive` with `path` and `language` (`en`/`pt`). `run` jobs also accept
`mode: "drive"` with `target: {kind: "export", out: "/drive"}` or `mode: "xbox"` with
`target: {kind: "install", host: "192.168.1.20"}`. Both use `games`, `keys`, optional
`titleKeys`/`firmware`, and `language`; drive completion includes `report`. Existing `game` and
`full` jobs remain supported.

`NXBOX_PACK_LANG` and `NXBOX_PACK_GITHUB_TOKEN` remain fallback aliases for `NXHELPER_LANG` and
`NXHELPER_GITHUB_TOKEN`. Saved credentials keep the existing service identity.
