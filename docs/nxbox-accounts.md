# NXbox accounts

A new install asks once whether to use multiple accounts. **No** is focused by default;
A accepts it and B also accepts No during initial setup. The answer is stored in
`LocalState/accounts.json` (`initialized`, `multi`, `selected_uuid`). Settings > Accounts
can change it later. Cancelling the Settings question preserves the previous mode.

Single mode uses the first stored account, hides the account list in NXbox, and exposes
only that account through the guest account list/count APIs. Switching modes keeps the
other accounts and their saves on disk. Multi mode shows the account picker before a
game launches, including protocol launches and direct boot; the last selected UUID is
focused. A confirms, B cancels the launch. Global/per-title `current_user` overrides
cannot replace the selected account. Account editing from the in-game Settings overlay
first returns to the library so the guest releases its save files.

Settings > Accounts supports up to eight profiles (the emulated console's limit): add,
select, rename, choose one of 20 bundled pictures, and remove. Names fit in 32 UTF-8
bytes. Removal first asks to remove the account; a second question defaults to keeping
its saves. Choosing Delete only this account's saves removes these two NAND directories:

- `user/save/0000000000000000/<UUID in U128 word order>` (legacy layout)
- `user/save/account/<UUID raw byte order>` (future layout)

Device, shared, system and other accounts' saves are untouched. Keeping saves leaves
those directories on disk; recreating an account generates a new UUID and does not
reattach orphaned saves automatically. Cloud copies are not deleted.

## Default avatar and migration

New profiles start as **NXbox**, with a 256×256 JPEG derived from the packaged UWP tile
`dist/nxbox/Assets/Square150x150Logo.png`. The avatar and 20 original geometric presets
are embedded in `src/core/constants_nxbox.h`. Regenerate with Python + Pillow 12.3.0:

```sh
python3 tools/nxbox/generate_avatars.py
```

An existing profile migrates only when there is exactly one stored profile, its name
is exactly `Player`, and its existing JPEG is byte-for-byte `ACCOUNT_BACKUP_JPEG`.
It becomes NXbox with the NXbox avatar while keeping its UUID. Renamed profiles,
custom images, absent images and multi-profile installations do not migrate. The
NXbox build also uses the NXbox image if an avatar cannot be read.

Profiles remain in Eden's binary `profiles.dat`. The exact avatar location is:

```text
LocalState/eden/nand/system/save/8000000000000010/su/avators/<formatted-uuid>.jpg
```

`avators` is intentionally spelled that way. UUID formatting follows
`Common::UUID::FormattedString()`, with lowercase hexadecimal and RFC-style hyphens;
it is different from the legacy save directory's U128 representation.

## NXbox Pack

```sh
nxbox-pack profile multi on
nxbox-pack profile multi off
nxbox-pack profile avatar "NXbox" ./picture.webp
```

The command discovers the Xbox, uses the existing Device Portal credentials/client,
and asks which installed NXbox package to edit. Each independent game app has its own
LocalState. Close that app before continuing and reopen it after the command finishes.
The optional `--insecure-portal` flag has the same meaning as in the pack workflow.
An avatar name must match exactly one existing profile; duplicate names are rejected.
Run the app once first so its profile database exists. Changing multi mode preserves
the stored UUID and unknown JSON fields, and marks initial setup answered.

PNG, JPEG, WebP, GIF and BMP files up to 20 MB are accepted. The command opens an
ephemeral loopback page in the PC's default browser, whose built-in decoders provide
RGBA pixels. It center-crops to a square and resizes to 256×256. Bun encodes baseline
color JPEG with the small pure-TypeScript encoder, reducing detail when necessary to
fit the guest service’s 128 KiB limit; no native image dependency is
added. Transparent pixels are composited over black. Animated images use the browser's
first decoded frame. The page is token-scoped, accepts only same-origin results and
closes its local server after completion or a two-minute timeout. A browser with
`createImageBitmap` support is required; the printed URL can be opened manually on
the same PC. Nothing is uploaded off the PC except the final JPEG sent to the Xbox.

Downloads use the documented Device Portal
[filesystem API](https://learn.microsoft.com/en-gb/previous-versions/windows/uwp/debug-test-perf/device-portal-api-core#download-a-file).
Avatar uploads never rewrite `profiles.dat` or choose a name-based filename.

## Validation

```sh
python3 -m unittest discover -s tests/port
cd tools/nxbox-pack && bun test && bun run check
```

The portable tests exercise initial-setup/picker policy, migration positive/negative
cases, avatar paths and embedded JPEG dimensions/uniqueness. Bun tests exercise binary
profile parsing, command validation, JSON preservation, UUID-targeted Portal transfers
and JPEG headers. The UWP app still needs MSVC CI and Xbox verification for rendering,
controller flows, native file replacement, guest account behavior and save isolation.
The state-file replacement uses the UWP-compatible
[ReplaceFileFromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-replacefilefromappw).
Browser decoding also needs an interactive PC smoke test for each supported format.

`NXBOX_UI_SCRIPT` uses the existing shared Input clock. On fresh LocalState, send an
initial A to accept single mode before scripting the existing save-sync setup/library.
For multi mode send Down, A instead; the launch picker then requires another A.
No separate timer or physical-controller-only polling was added.
