# NXbox compatibility runner

Run the probe-style console loop using Python 3's standard library. It does not
build, deploy, change console accounts, or depend on the untracked local scripts.
Run from the repository root:

```sh
python3 -m tools.nxbox.compat.run --games games.json \
  --seconds 90 --output /path/to/new-run-directory
```

The input is a JSON array. Games must already exist on the console:

```json
[
  {
    "title_id": "01007EF00011E000",
    "name": "The Legend of Zelda: Breath of the Wild",
    "file_name": "The Legend of Zelda Breath of the Wild [01007EF00011E000][v0] (13.48 GB).nsp"
  },
  {
    "title_id": "010028600EBDA000",
    "file_name": "games\\Super Mario 3D World.nsp",
    "input_script": "10:L+R:2,15:A:1"
  }
]
```

Bare filenames are prefixed with `games\`; explicit relative paths or drive paths
are passed through to `game.txt`. Title IDs identify report entries; the selected
package reads the actual game title ID at boot. Duplicate IDs are rejected.

Set `NXBOX_PORTAL_HOST`, `NXBOX_PORTAL_USER`, and `NXBOX_PORTAL_PASS` in the
process environment. On macOS, missing values fall back to the JSON password
record in keychain service `claude-autonomous:XBDEV` (`host`, `user`, `pass`).
The host accepts an optional HTTPS scheme and port, defaulting to HTTPS 11443.
The client accepts the console's self-signed TLS certificate, sends Basic auth,
obtains the CSRF cookie, and rejects redirects. Credentials and response bodies
are never printed. No credential files or local helper processes are required.

Options:

- `--package`: installed package name/full name; default `NXbox` selects the
  newest installed `NSPX.NXbox_` version.
- `--env-file`: environment overrides to push as `nxbox_env.txt`. The runner
  appends `NXBOX_FRAME_DUMP=1` so frame capture is enabled.
- `--input-script`: global input sequence, also defaulted from
  `NXBOX_INPUT_SCRIPT`. A game's `input_script` takes precedence.
- `--poll`: diagnostic polling interval in seconds (default 1).
- `--max-frames`: capture limit per game (default 40). The JSON also records the
  available frame count. Only valid binary RGB P6/255 dumps enter contact sheets.

The runner waits for `/tmp/xbox-console.lock`, including an empty lock, and
creates its own lock atomically. It never reclaims a stale or foreign lock. It
checks its ownership token and inode before removal. Existing local scripts must
continue honoring the same lock; scripts that overwrite an active lock cannot
be serialized by this runner.

For each game it stops the selected package, archives previous diagnostics,
crash evidence, Eden log and frame dumps, clears those remote artifacts, pushes
`LocalState/nxbox_env.txt`, `game.txt`, and `skip_library.txt`, launches, waits,
pulls evidence and stops again. Archive data is excluded from scores. Errors
in collection are recorded; connection/authentication failures are not treated
as missing files. Failure to stop prevents launching the next game. Existing
control files are backed up under `previous-controls/` and restored on normal
completion, exceptions, Ctrl-C or SIGTERM. Failed restoration is recorded in
`cleanup-errors.json`. SIGKILL or loss of power cannot run cleanup; the owned
lock and backups remain for manual recovery.

Outputs are `compat.md`, `compat.json`, per-title raw diagnostics, Eden logs,
`nxbox_crash.txt` when present, PPM frame dumps and `contact-sheet.png`. The PNG
writer uses only `struct` and `zlib`; sheets preserve aspect ratio, use nearest
neighbor resizing, and list tile filenames in reading order in Markdown.
An empty capture still produces a black sheet explicitly labeled as empty.
The output directory must be new. Reports are refreshed after each game.

`GAME_RUNNING` establishes boot success. `GAME_FIRST_FRAME` has no timestamp,
so first-frame seconds are the launch-to-poll observation upper bound, including
portal latency; absent evidence is `null`, never zero. Each `GAME_PRESENT`
sample supplies an interval frame count (not a cumulative counter), actual
interval duration, normalized presents/5 seconds and console tick when available.
Timeline windows accumulate those durations from the runtime's first measurement,
not host launch; suspension time is excluded. Memory peak is the maximum sampled
`GAME_PRESENT memory` byte count or `MEM commit` MiB measurement. The tool cannot
recover unsampled peaks or establish visual correctness from presentation counts.

The deterministic score is 0–100:

- 20 for `GAME_RUNNING`, 20 for first-frame evidence.
- Up to 30 for the fraction of measurement windows with nonzero presents.
- Up to 20 for aggregate presentation FPS relative to 30 FPS.
- 10 for at least one valid frame dump.
- `GAME_FAIL`, load/missing failures or a nonempty crash file cap the score at 20.
- Runner failures score zero. Collection errors remain visible in the report.

Exit status is 0 for a completed collection, 1 for runner/collection/cleanup
failures, and 130 for interruption. Game failures themselves are report outcomes,
not transport failures.

Host validation (does not access the console or modify `/tmp/mesa-pin`):

```sh
cd tests/port && python3 -m unittest discover -s . -p 'test_*.py'
```
