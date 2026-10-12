# Unattended Xbox compatibility matrix

From the repository root, test the installed library against both renderers with one command:

```sh
python3 -m tools.nxbox.compat.matrix --sha ff72a7f02 --appx /path/to/main-build.appx
```

The caller supplies a package built from the given main commit (and optionally repeated
`--dependency /path/to/dependency.appx` arguments for dependencies/certificates).
The SHA is a caller assertion; the report also records the package SHA-256 and installed
package identity. This tool neither builds nor downloads artifacts nor uses game data in git.
Live screenshot decoding needs Pillow (`python3 -m pip install Pillow`). Portal credentials
come from `NXBOX_PORTAL_HOST`, `NXBOX_PORTAL_USER`, `NXBOX_PORTAL_PASS` or the existing macOS
`claude-autonomous:XBDEV` keychain record. Set HOST explicitly if the console address changed.

To exercise the complete reporting pipeline without a console, credentials or a lock:

```sh
python3 -m tools.nxbox.compat.matrix --sha ff72a7f02 --dry-run
```

Dry-run uses a tiny synthetic title and synthetic diagnostics; reports are explicitly marked
and compared only against other dry-runs. It does not validate deployment or console behavior.
`--artifacts /path/to/artifacts` redirects output; `--seconds 180` selects each case's duration;
`--renderers opengl` or `--renderers d3d12` restricts the matrix. The default tests both.

## Console ownership and recovery

Live runs wait for `/tmp/xbox-console.lock`, including empty foreign locks. Acquisition uses
exclusive creation; release checks both inode and a unique token. Foreign/stale locks are
never stolen or deleted. All console operations, including installation and restoration,
run inside that ownership interval. SIGTERM and Ctrl-C unwind cleanup and release ownership;
SIGKILL or power loss leaves the lock for its owner to inspect. This assumes all console users
honor the same lock protocol; it cannot protect against tools that overwrite foreign locks.

The runner stops existing NXbox processes, installs the supplied package, waits for asynchronous
deployment, selects the exact manifest name/version/architecture of the supplied NXbox package, then enumerates `LocalState/library/<TITLEID>.json`
records plus every `.nsp`, `.xci`, `.nsz` in `LocalState/games`. Library records provide actual IDs,
names and absolute paths. Uncached filenames without an ID get a stable `unknown-<hash>` identity;
they are still tested. Package-path identities distinguish multiple files for a single title.
Updates/DLC or compressed packages can fail to boot and are reported rather than silently excluded.
Cached removable-drive paths are attempted and flagged; uncached files on inaccessible external
storage cannot be discovered through the app sandbox endpoint. This is discovery of accessible
packages, not a parser for encrypted game contents or a claim that all cache entries remain installed.

Full crash-dump collection is disabled for NXbox before testing, matching the local preflight
policy; stored dumps are retained and collection remains disabled afterward. No account or
certificate settings are changed. Previous launch controls, including `game.url`, global settings
and existing per-title settings, are backed up in `previous-controls/` and restored byte-for-byte
in `finally`. Both settings labels (`backend` and `renderer_backend`) are overridden temporarily
with OpenGL SPIR-V, the UWP default. D3D12 is selected through `NXBOX_RENDERER=d3d12`.
A stop failure aborts the matrix instead of starting another case. Cleanup failures produce an
incomplete report and nonzero exit; backups remain for manual recovery. Installation itself is
not rolled back. Generic input can change saves or activate menu selections: use test saves.

## Evidence and interpretation

Each case presses A and PLUS/Start periodically and holds the right stick after 45 seconds
using the existing in-app `NXBOX_INPUT_SCRIPT` protocol. No title-specific scripts are needed.
`NXBOX_THREAD_PROFILE=1` enables CPU/wait telemetry. Guest frame dumping is disabled to avoid
unbounded on-console dumps. Device Portal screenshots are captured at launch and roughly every
30 seconds (portal latency can delay them), decoded to PPM for sheets and sampled at 32x18 for
pixel changes. Raw screenshots and logs are retained locally. Failed captures are recorded.

The runner polls diagnostics every five seconds, archives/clears previous logs before each case,
collects `eden_uwp_diag.txt`, `eden/log/eden_log.txt`, `nxbox_crash.txt`, then stops the app. Reports
include every observed GAME_PRESENT interval with FPS and normalized presents per five seconds,
raw THREAD_PROFILE lines (thread names, CPU percentages and host waits), fatal/recovered exception
lines including `what()`, and InfoQueue counts grouped by reported severity. InfoQueue counts are
observed log message counts, including duplicates, not device queue totals. Missing telemetry is
unknown; `infoqueue_available` distinguishes absence from observed messages. Renderer crashes are
evidence, not runner infrastructure failures; the latter cause a nonzero exit. A successful exit
means the matrix completed without collection errors, not that every game passed.

States have explicit reasons:

- **boot**: no positive presentation interval, including missing diagnostics.
- **menus**: presentation observed without enough sustained frame-change evidence.
- **in-game**: at least three positive intervals averaging at least 10 FPS, plus two capture
  transitions with mean RGB difference at least 8%. Animated menus and overlays can satisfy this
  heuristic; static gameplay can remain classified as menus. This is not proof of playability.
- **crashed**: GAME_FAIL/load failure/missing marker, terminate, unhandled exception,
  D3D12_REMOVED, or nonempty crash file. A recovered `what()` line alone does not imply a crash.
- **error**: runner failure, with game state unknown.

## Artifacts and comparison

Every invocation writes `~/.local/share/nxbox/artifacts/matrix-<sha>-<UTC-date>/` with `compat.md`,
`compat.json` (schema 2), `diff.json`, a matrix overview contact sheet, per-case logs/screenshots/contact sheets, and launch-control
backups. Reports are checkpointed after each case and on interruption; `complete` records whether
all cases and cleanup finished. Do not commit these artifacts: they can contain private paths,
URLs or game screenshots. The newest schema-2 run of the same live/dry-run type is the baseline.
Regressions are highlighted for lower state rank, FPS drops over 20%, or higher observed InfoQueue
counts. New and missing cases are separate changes. Duration/settings can differ between runs;
interpret the heuristic diff with the evidence, especially when comparing menus versus gameplay.

Host verification (no console access):

```sh
cd tests/port
python3 -m unittest discover -s . -p 'test_t13_compat*.py'
python3 -m unittest discover -s . -p 'test_*.py'
```

The fixtures exercise inventory, native log parsing, recovered versus fatal exceptions, scene
classification, input generation, ownership, asynchronous installation, dry-run reports and diffs.
