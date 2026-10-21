# PipenSX compatibility audit — 2026-10-21

The three NRO review findings are fixed and covered by host regressions.
The official PipenSX 1.6.4 asset passes the bounded metadata constructor.
PipenSX runtime compatibility on Xbox is **not established**. In particular,
its installation backend calls NCM commands that NXBox does not implement.

## Revisions and evidence boundary

| Component | Revision | What was inspected |
| --- | --- | --- |
| NXBox before fixes | `e55391d7c11812221cdf9a495e06e0ca0b1d9475` | PR #2 and the existing guest services |
| PipenSX 1.6.4 | [`b44e914dfee75a9ae5142637364222172a612f37`](https://github.com/i3sey/pipensx/tree/b44e914dfee75a9ae5142637364222172a612f37) | Release tag, Switch startup, networking and install backend |
| PipenSX Borealis submodule | [`b1fa20d809ee3eeb05af8b7a3661ce3cb098635e`](https://github.com/i3sey/borealis/tree/b1fa20d809ee3eeb05af8b7a3661ce3cb098635e) | Switch platform and deko3d renderer |
| PipenSX libnx-ext submodule | `ddd3977033f81af583fe05d34a8feacb09c0c3e9` | Pinned dependency identity |
| libnx reference | [`b156b4774fb2df7b93402169509266da7547fc4c`](https://github.com/switchbrew/libnx/tree/b156b4774fb2df7b93402169509266da7547fc4c) | Homebrew ABI, default startup, RomFS selection and NCM command numbers |

The reference libnx revision is a source reference, not an identification of
the library linked into the published NRO. No ARM64 guest code was executed
by the metadata probes. No catalog, torrent, download or installation was run.

## Review fixes

1. `AppLoader_NRO` now checks the NRO/ASET boundary and each optional asset
   against the actual file length using subtraction before adding offsets.
   Asset ranges cannot overlap the ASET header. NACP must contain the complete
   `RawNACP`; icons are limited to 1 MiB, including physically present assets.
   Invalid optional assets are ignored independently, preserving valid metadata.
2. The library catches both WinRT metadata errors and standard exceptions for
   each NRO. A failed entry is skipped and counted; later entries remain available.
3. `LibraryFiles` permits NRO deletion and transfer through the existing path,
   root and reparse-point guards. NRO and NSP/NSZ/XCI entries with the same stem
   are kept separate when selecting companion files.
4. The scan harness supplies the new VFS, loader and WinRT interfaces. It exercises
   filename fallback, metadata extraction, stable/distinct identities, invalid
   files and failure isolation. It accepts either Clang or GCC.

The constructor regression executes the production constructor and identifier
with counted, bounded VFS and minimal NACP/offset-file substitutes. It verifies
asset selection and allocation bounds, not the full NACP language parser.
It covers oversized sizes, wrapped or
out-of-file offsets, incomplete NACP, absent ASET, valid assets and a present
icon exceeding the cap. It is not a complete validator of executable NRO segments.

## Validation

Host environment: Windows/MSVC 19.51 with Windows SDK 10.0.26100; Linux under
WSL with GCC 15.2.0. The C++ host harnesses compile with assertions enabled and
`-Wall -Wextra -Werror`.

| Check | Result |
| --- | --- |
| `python3 -B -m unittest discover -s tests/port -p test_nro_metadata.py -v` | 1 passed |
| `python3 -B -m unittest discover -s tests/port -p 'test_library_*.py' -v` | 19 passed |
| Native `tests/port` CMake build and CTest, MSVC Debug | 10 passed; assertions enabled |
| Native `tests/port` CMake build and CTest, MSVC Release | 10 passed; supplementary build check |
| New constructor regression against pre-fix source | Rejected: unbounded asset read |
| New per-entry failure regression against pre-fix source | Rejected: uncaught `std::bad_alloc` |
| NRO deletion/transfer eligibility against pre-fix header | Rejected |
| Official PipenSX NRO through the fixed production constructor | Icon 9,685 bytes, NACP present, RomFS present, one bounded icon read |

Native configure/build commands:

```powershell
cmake -S tests/port -B build-port-tests -G "Visual Studio 18 2026" -A x64
cmake --build build-port-tests --config Debug --parallel 2
ctest --test-dir build-port-tests -C Debug --output-on-failure
```

The NRO and library Python suites are also registered with CTest on Unix hosts.
The port workflow now triggers for the NRO loader and library source/header
changes. A complete Xbox frontend/Appx build and console test remain separate gates.

The UWP preset was attempted in an initialized `vcvarsall x64 uwp` environment
with the installed Store CRT. Configuration reached OpenSSL 3.6.2 and stopped
at `ConfigureOpenSSL.cmake:128`: `OPENSSL_CONFIGURE_TOOL` could not find native
`perl`. No full frontend compile, link or Appx was produced. The host builds
above do not replace that gate; see `docs/uwp_build.md` for Perl/NASM prerequisites.

## Official release inspected

Asset: [PipenSX 1.6.4 `pipensx.nro`](https://github.com/i3sey/pipensx/releases/tag/1.6.4),
published 2026-10-17. Its SHA-256 matches both the GitHub asset digest and the
release checksum file:

```text
efcaa30335ff332ed539743b24f91e115e814c2ba70fd0ef8cd26763a25b7394
```

| Field | Value |
| --- | --- |
| File size | 47,889,621 bytes |
| Executable NRO region | 5,328,896 bytes |
| Header / asset version | `NRO0` / `ASET` version 0 |
| Icon relative offset / size | 56 / 9,685 bytes |
| NACP relative offset / size | 9,741 / 16,384 bytes |
| RomFS relative offset / size | 26,125 / 42,534,600 bytes |

All three asset ranges fit within this file. The new cap therefore does not
exclude this release. This establishes metadata acceptance, not a boot result.

## Runtime compatibility map

| Stage | Source evidence | Assessment |
| --- | --- | --- |
| Application mode | PipenSX rejects modes other than Application/SystemApplication. The reference libnx default resolves to Application. | No source-level proof that this mode check necessarily fails; verify the logged type on Xbox. |
| Homebrew ABI and RomFS | NXBox emits a homebrew configuration table only when `program_args` is nonempty, using `homebrew` as argv[0]. With no table, reference libnx selects its NSO/current-process RomFS path; NXBox has a loader RomFS factory and FSP command 200. | Test the default/no-arguments launch first. A nonempty argument setting does not provide a usable guest NRO pathname automatically. A proper homebrew argv path must be accessible inside the virtual SD filesystem. |
| SD persistence | FSP command 18 opens `SDMCFactory`; its directory comes from `EdenPath::SDMCDir`, under UWP `LocalState/eden/sdmc`. PipenSX writes `sdmc:/switch/pipensx`. | A virtual writable SD implementation exists. Uploading an NRO to `LocalState/games` does not itself place it at a guest SD path. Persistence, commit behavior and capacity must be tested across restarts. |
| Startup services | `userAppInit` initializes sockets, RomFS, HID/system settings, power, NIFM and backlight. `main` initializes NCM, NS and ES before Borealis. NXBox registers these service families, including `ins:s`. | Service names and several initialization handlers exist. Registration alone does not prove each libnx initialization sequence succeeds. Capture the first failed command. |
| Borealis graphics/input | The pinned PipenSX build forces `USE_DEKO3D=ON`. Borealis creates deko3d device/queue, GPU memory pools, framebuffers and a NanoVG context. | Requires emulated NV services, command execution, shaders, presentation, fonts and controller input. Existing game rendering does not prove this UI works. No Xbox UI result is available. |
| TCP/UDP/DNS/HTTPS | NXBox has BSD registration, sockets, polling, connect/send/receive and DNS handlers. PipenSX uses 12 BSD sessions, below the `bsd:u` limit of 15; `RegisterClient` currently ignores transfer-memory sizing. Several socket options return stubbed success. PipenSX links curl and mbedTLS. | Source support exists, with semantic gaps. Test guest DNS, TCP, TLS/certificates, polling and UDP independently; a host HTTP request or an SSL-service unit test is not evidence for PipenSX's guest mbedTLS path. |
| Content installation | NCM opens storage/database interfaces and implements several placeholder/write/register operations, NS has record refresh, ES has ticket import. Required queries are missing; details below. | The existing implementation cannot establish full PipenSX installation compatibility. Auto-stub success does not supply missing data or durable effects. |
| Self-relocation/update/relaunch | PipenSX uses argv[0], guest SD files and `envHasNextLoad`/`envSetNextLoad`. NXBox's configuration table does not provide NextLoadPath. | Relaunch/update behavior is unsupported by this ABI path; initial UI startup must be evaluated separately. |

Primary startup sources:
[PipenSX main](https://github.com/i3sey/pipensx/blob/b44e914dfee75a9ae5142637364222172a612f37/src/main_switch.cpp),
[userAppInit](https://github.com/i3sey/pipensx/blob/b44e914dfee75a9ae5142637364222172a612f37/src/platform/switch_runtime.c),
[Borealis deko3d setup](https://github.com/i3sey/borealis/blob/b1fa20d809ee3eeb05af8b7a3661ce3cb098635e/library/lib/platforms/switch/switch_video.cpp),
[libnx environment](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/source/runtime/env.c),
[libnx RomFS selection](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/source/runtime/devices/romfs_dev.c).

## Confirmed installation gaps

The following calls occur in PipenSX's
[`install_backend_switch.cpp`](https://github.com/i3sey/pipensx/blob/b44e914dfee75a9ae5142637364222172a612f37/src/install/install_backend_switch.cpp)
or installed-title enumeration. Command numbers are checked against the
[libnx NCM source](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/source/services/ncm.c).
They have no handler in NXBox's `src/core/hle/service/ncm/ncm.cpp`:

| Interface | Command | libnx operation | Consequence |
| --- | --- | --- | --- |
| IContentStorage | 3 | HasPlaceHolder | Resume/registration recovery cannot query placeholder existence. |
| IContentStorage | 7 | Has | NCA setup returns an error when querying existing content. |
| IContentStorage | 8 | GetPath | The backend cannot obtain the content path used for inspection. |
| IContentStorage | 22 | GetFreeSpaceSize | Installation cannot perform its required space check. |
| IContentStorage | 25 | GetSizeFromPlaceHolderId | Resume/diagnostic size queries cannot be satisfied. |
| IContentMetaDatabase | 5 | List | Installed-version enumeration cannot use the expected database API. |

`IContentMetaDatabase::Set` currently retains keys in a per-interface vector;
it does not persist the supplied metadata buffer. `Commit` refreshes the
registered cache. Those operations do not demonstrate a persistent Horizon-style
metadata database. `PushApplicationRecord` similarly refreshes caches and signals
an event; that is not proof of all record-management semantics.

NXBox logs missing calls with `NXBOX_SERVICE_MISSING`. With `use_auto_stub`
enabled it can return success for a missing command, so a successful IPC result
must be paired with valid output and verified durable state.

## Next console experiment

1. Build/package this branch with the UWP frontend. Keep an existing LocalState
   backup and record the NXBox commit, Appx hash, console model, settings and NRO hash.
2. Use the verified 1.6.4 NRO, default empty `program_args`, and a fresh virtual
   `sdmc:/switch/pipensx` test directory. Do not infer a guest path from the host
   library path. Record whether the current-process RomFS path opens resources.
3. Capture startup through `Borealis createWindow` and an interactive rendered
   frame. Retrieve NXBox diagnostics, crash report, guest SD PipenSX logs and
   the first `NXBOX_SERVICE_MISSING` tuple, including interface and command.
4. Only after UI acceptance, test a small SD marker across a clean restart and
   one controlled guest HTTPS request. Keep catalog refresh/download workers
   out of the initial isolated networking result.
5. Treat installation as a later implementation milestone: add bounded virtual
   NCM queries and persistent metadata first, then use synthetic packages to
   verify failure/rollback, content registration and restart recovery.

No Series S UI, guest HTTPS, download, installation or restart-persistence
acceptance is claimed by this audit.
