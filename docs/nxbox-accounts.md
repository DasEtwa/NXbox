# NXbox accounts

A new install starts immediately in single-account mode with the NXbox logo. There is no
first-launch account question. Near the bottom of Settings, the **Account** section contains
**Profile picture**, **Multiple accounts** (off by default), and **My Morus**. Profile picture opens
a character-or-Moru choice in either account mode. Multiple accounts opens the account management
list, where its first row toggles the mode. The compatible `LocalState/accounts.json` format still
stores `initialized`, `multi`, and `selected_uuid`; `initialized` no longer gates UI.

Single mode uses the first stored account, hides the account list in NXbox, and exposes only that
account through the guest account list/count APIs. Switching modes keeps the other accounts and
their saves on disk. Multi mode shows the account picker before a game launches, including protocol
launches and direct boot; the last selected UUID is focused. A confirms, B cancels the launch.
Global/per-title `current_user` overrides cannot replace the selected account. Account editing from
the in-game Settings overlay first returns to the library so the guest releases its save files.

Settings > Account > Multiple accounts supports up to eight profiles (the emulated console's limit):
add, select, rename, choose an original character and background color, and remove. Names fit in 32
UTF-8 bytes. Removal first asks to remove the account; a second question defaults to keeping its
saves. Choosing Delete only this account's saves removes these two NAND directories:

- `user/save/0000000000000000/<UUID in U128 word order>` (legacy layout)
- `user/save/account/<UUID raw byte order>` (future layout)

Device, shared, system and other accounts' saves are untouched. Keeping saves leaves those
directories on disk; recreating an account generates a new UUID and does not reattach orphaned saves
automatically. Cloud copies are not deleted.

## Default avatar and migration

New profiles start as **NXbox**, with a 256×256 JPEG derived from the packaged UWP tile
`dist/nxbox/Assets/Square150x150Logo.png`. The fixed logo remains byte-for-byte unchanged. 120
original soft 3D character portraits are stored as transparent 512×512 PNG masters in
`tools/nxbox/avatars/`. The art was produced with OpenAI's built-in image generator, one prompt per
character; `prompts.json` archives the production prompts. `avatars/regenerate_art.js` is a Codex
`functions.exec` orchestration script that calls the built-in image generator for each portrait and
rebuilds the assets. Run it from the repository root in that runtime; it is not a Node/Bun script
and needs no API key. Art generation is stochastic; review every replacement on the contact sheet.
No existing characters or reference images were used. Artwork is licensed to NSPX under MIT
(`avatars/LICENSE.md`).

`src/core/constants_nxbox.h` embeds only the fixed NXbox logo; character metadata and forty named
backgrounds remain small constexpr tables. `dist/nxbox/Assets/avatars/<id>.png` contains the 120
optimized transparent 256×256 runtime portraits. The UWP frontend loads them from
`Package::Current().InstalledLocation()`. CMake stages that directory after an MSVC build;
`package_uwp.py` copies it into the appx, and nxbox-pack preserves it in per-game packages. Pillow's
lossless PNG optimizer is used (no pngquant or oxipng is installed in the authoring environment).
`avatars/catalog.json` assigns every stable character ID to Animals, Fantasy, Sci-fi, Sports, Food,
Monsters, Robots, or People. The retained `fox-pilot` ID now depicts a lavender-aproned ceramic
artisan, without aviation clothing or accessories. The palette keeps the original twelve colors,
adds pastels and darks, and includes four vertical two-tone gradients: sunrise, lagoon, dusk, and
ember. Each gradient interpolates RGB with integer rounding from top to bottom.
`tools/nxbox-pack/src/avatar-catalog.json` carries the same downsampled pixels as compressed
premultiplied RGBA for standalone PC binaries. Regenerate both catalogues and `avatars/sheet.png`
from the PNG masters with Python + Pillow 12.3.0:

```sh
python3 tools/nxbox/generate_avatars.py
```

An existing profile migrates only when there is exactly one stored profile, its name is exactly
`Player`, and its existing JPEG is byte-for-byte `ACCOUNT_BACKUP_JPEG`. It becomes NXbox with the
NXbox avatar while keeping its UUID. Renamed profiles, custom images, absent images and
multi-profile installations do not migrate. The NXbox build also uses the NXbox image if an avatar
cannot be read.

Profiles remain in Eden's binary `profiles.dat`. The exact avatar location is:

```text
LocalState/eden/nand/system/save/8000000000000010/su/avators/<formatted-uuid>.jpg
```

`avators` is intentionally spelled that way. UUID formatting follows
`Common::UUID::FormattedString()`, with lowercase hexadecimal and RFC-style hyphens; it is different
from the legacy save directory's U128 representation.

## NxHelper

```sh
nxbox-pack profile multi on
nxbox-pack profile multi off
nxbox-pack profile avatar "NXbox" ./picture.webp
nxbox-pack profile avatar "Miguel" --character fox-pilot --color sky-blue
```

The command discovers the Xbox, uses the existing Device Portal credentials/client, and asks which
installed NXbox package to edit. Each independent game app has its own LocalState. Close that app
before continuing and reopen it after the command finishes. The optional `--insecure-portal` flag
has the same meaning as in the pack workflow. An avatar name must match exactly one existing
profile; duplicate names are rejected. Run the app once first so its profile database exists.
Changing multi mode preserves the stored UUID and unknown JSON fields, and keeps the legacy
`initialized` field set for compatibility.

Character choices work offline without a browser or native decoder. All 120 stable IDs and
categories are listed in [`catalog.json`](../tools/nxbox/avatars/catalog.json); forty color names
and their RGB endpoints are listed in [`generate_avatars.py`](../tools/nxbox/generate_avatars.py).
Both flags are required; either order is accepted. Unknown IDs/colors and repeated flags are
rejected.

For custom images, PNG, JPEG, WebP, GIF and BMP files up to 20 MB are accepted. The command opens an
ephemeral loopback page in the PC's default browser, whose built-in decoders provide RGBA pixels. It
center-crops to a square and resizes to 256×256. Bun encodes baseline color JPEG with the small
pure-TypeScript encoder, reducing detail when necessary to fit the guest service’s 128 KiB limit; no
native image dependency is added. Transparent pixels are composited over black. Animated images use
the browser's first decoded frame. The page is token-scoped, accepts only same-origin results and
closes its local server after completion or a two-minute timeout. A browser with `createImageBitmap`
support is required; the printed URL can be opened manually on the same PC. Nothing is uploaded off
the PC except the final JPEG sent to the Xbox.

Downloads use the documented Device Portal
[filesystem API](https://learn.microsoft.com/en-gb/previous-versions/windows/uwp/debug-test-perf/device-portal-api-core#download-a-file).
Avatar uploads never rewrite `profiles.dat` or choose a name-based filename.

## Validation

```sh
python3 -m unittest discover -s tests/port
cd tools/nxbox-pack && bun test && bun run check
```

The portable tests exercise no-prompt/picker policy, migration positive/negative cases, avatar
paths and embedded PNG dimensions/uniqueness, transparent masters, catalogue counts, and alpha-over
pixel fixtures. Bun tests exercise binary profile parsing, command validation, JSON preservation,
UUID-targeted Portal transfers and JPEG headers. The UWP app still needs MSVC CI and Xbox
verification for rendering, controller flows, native file replacement, guest account behavior and
save isolation. The state-file replacement uses the UWP-compatible
[ReplaceFileFromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-replacefilefromappw).
Browser decoding also needs an interactive PC smoke test for each supported format.

`NXBOX_UI_SCRIPT` uses the existing shared Input clock. Fresh LocalState goes straight to the
existing save-sync setup/library, with no initial account-mode input. Settings > Account > Profile
picture > Choose a character opens the six-column grid in both account modes. LB/RB switches the
eight visible category tabs; D-pad moves the ring and scrolls larger categories. A chooses a
character. The next page shows a composite preview and forty named swatches in a 10×4 grid.
Left/Right/Up/Down update the preview, including gradients; A writes a 256×256 JPEG to the existing
UUID path. B returns without writing. The Moru option uses the same background picker and
confirmation path. New accounts keep the NXbox logo until a composite is confirmed.

The console decodes the PNG using the renderer's WIC path (premultiplied BGRA), composites each
channel as `min(255, foreground + (background*(255-alpha)+127)/255)`, and uses the existing stb JPEG
encoder, reducing quality to stay within 128 KiB. PC uses the same premultiplied pixel formula and
its existing TypeScript JPEG encoder; JPEG bytes may differ, while the input composite pixels match.

Example scripted editing from the multi-account Settings list: Down, A (first account), Down, Down,
A (Change icon), Right, A (second character), Right, A (orange background and save). Use the
existing `NXBOX_UI_SCRIPT` timing syntax. No separate timer or physical-controller-only polling was
added.

## Moru

**Settings > Account > My Morus** opens the system character collection in either account mode. In
either mode, Profile picture offers **Use a Moru as icon**. In multi-account mode, each account's
detail page also offers **My Morus** and **Use a Moru as icon**. Morus belong to the emulated
system, rather than to an individual account. The collection shows up to eight rows at once and
scrolls through the database. Create opens a draft; A edits an existing entry, or chooses it when
opened as an icon picker. X always edits an existing entry. Up/Down selects a category, Left/Right
changes its value with a live preview; A also advances a part choice. Gender, height and build have
slider tracks. Nickname opens the existing on-screen keyboard. Random changes parts and proportions
while retaining the nickname and identity. Save persists; Discard and Delete require a separate
confirmation. B from the editor opens the discard confirmation. All input uses the shared `Input`,
including `NXBOX_UI_SCRIPT`, with no independent polling or timer.

The editor's original 2D rounded shapes are defined in `moru_parts.h`; no console assets, fonts,
sounds or logos are used. One 256-unit scene drives both the existing Direct2D renderer and a
supersampled premultiplied BGRA raster for icons. Selecting a saved Moru opens the forty-colour
background picker. Confirmation uses exactly the character avatar's alpha-over formula, stb JPEG
quality reduction, 128 KiB limit and UUID-based `SetAvatar` path. Cancelling the picker or
background step does not write a profile image. The icon is a snapshot; later edits or deletion of
the Moru do not change an already saved profile JPEG.

### Stored data and ranges

There is no parallel JSON character store. The editor uses the service's `DatabaseManager`
with ordinary session metadata (special entries remain hidden), `StoreData` setters and
`CharInfo::Verify`. New entries use NXbox's own `BuildDefault(0)`, which calls the existing
create-id utility. Editing copies the complete stored entry and retains its create-id.
Only the selected field changes; choosing a hair colour also synchronizes eyebrow and beard
colour. Imported parts outside the editor catalogue display **Original** and remain intact
until that category is changed. The simplified preview approximates those imported parts.

Catalogue positions map to these service IDs, in order:

| Category | Stored IDs / inclusive range |
| --- | --- |
| Face | 0, 1, 4, 9 |
| Skin | 0–9 |
| Hair | 1, 8, 39, 7, 0, 14, 6, 129 |
| Hair / eyebrow / beard colour | 8, 1, 2, 3, 4, 5, 6, 7 |
| Eyebrows | 16, 5, 15, 23 |
| Eyes | 0, 9, 10, 56 |
| Eye colour | 8, 9, 10, 11, 12, 13 |
| Nose | 2, 6, 1, 8 |
| Mouth | 0, 16, 4, 30 |
| Beard | 0, 1, 5 |
| Moustache | 0, 2, 4 |
| Glasses | 0–3 |
| Glasses colour | 8, 14, 15, 16, 17, 18, 0 |
| Favourite colour | 0–11 |
| Gender | 0–1 |
| Height / build | 0–127 each |
| Nickname | 1–10 UTF-16 code units, zero padded |

These limits come from the existing service enums and validation, not the bitfield widths.
Common colours are valid up to 99, but this editor uses the service's legacy colour subsets.
The preview uses RGB approximations in legacy order, following `RawData::Ver3*ColorTable`
in `src/core/hle/service/mii/types/raw_data.cpp`. Hair IDs 8/1/2/3/4/5/6/7 are
black/brown/red/light brown/grey/olive/auburn/blonde; stored ID 8 represents legacy black,
while ID 7 is blonde, not white. Eyebrows and facial hair read their own stored colours.
`CharInfo` copies these IDs without remapping. The HLE has no RGB rendering table;
hair uses the documented approximations, and the other legacy RGB sets follow the
[FFL colour implementation](https://github.com/aboood40091/ffl/blob/master/src/FFLiColor.cpp).
Only colour values are used; all shapes remain original rounded 2D artwork.

Skin IDs 0–5 follow the legacy six-tone set. In particular, ID 5 is dark brown in that
set despite the service enum's `Porcelain` label. Extended skin IDs 6–9 remain estimates
based on the service enum names; these need confirmation in a Switch game. Glasses ID 0
is the extended white choice, distinct from legacy black (stored ID 8). Lighting and
materials in games can change the apparent RGB. The eight catalogue hair silhouettes
are short, parted short, afro, front lock, long, peaked top, parted long and two side pony
tails, respectively; both long styles extend to the shoulders. Imported styles outside
the catalogue remain approximate. Nicknames reject controls and unpaired UTF-16
surrogates; characters outside the BMP consume two units.

Every change refreshes the data and device CRC through `StoreData::SetChecksum`. Save and delete
use `AddOrReplace` / `Delete`, then `SaveDatabase`, writing the same
`nand/system/save/8000000000000030/MiiDatabase.dat` that games read. The entire database is limited
to 100 entries, including hidden special entries. Replacement still works at capacity. The
integrity check now accepts exactly 100 entries on reload. Load errors disable edits and expose
retry instead of saving an empty replacement over a corrupt database. Editing is available only
after the guest has released the database; in-game Settings first returns to NXbox.

`test_accounts_moru.py` compiles the production character packing, UUID, CRC, database manager and
file I/O with a host C++20 compiler and fmt headers. Only logging and the NAND path/filesystem
helpers are replaced by test infrastructure. Tests cover every choice, packed round trips,
validation, CRC corruption, retained IDs/imported fields, nicknames, persistence at capacity,
replacement/deletion and the shared icon raster. They also scan all UI language files and
`strings.h` for the reserved character name. MSVC CI and an Xbox run remain necessary to verify
layout, controller/script navigation, profile JPEG display and discovery by games such as
Mario Kart 8; a passing host test does not establish game-specific rendering compatibility.
