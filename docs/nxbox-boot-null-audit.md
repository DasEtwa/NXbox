# TotK and ACNH boot null dereferences (2026-10-18 captures)

The captures establish guest null dereferences and an earlier shared RomFS integrity
failure. They do **not** establish that a missing service or a success-with-null IPC
response caused either crash. A real success-with-incomplete-data bug was found and
fixed in FSP `IStorage::Read`, but its involvement in these captures is unproven.
Neither game has been retested on Xbox with this change.

Evidence was read without modification from
`/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/t-totk/`
and `t-acnh/` (`eden.txt` and `diag.txt`). Line references below are to `eden.txt`.
The desktop comparison below is a comparison of shared source paths, not a new
desktop execution; no desktop trace or guest binaries were supplied.

## Tears of the Kingdom, 0100F2C0115B6000, update 720896

The first unmapped read is at **10.767606 s**, line 356, not 19.7 s. The later
timestamp belongs to the same continuing storm. The fault context at lines 288–355
records PC `0x82f66fb4`, LR `0x80d8d770`, and `x8=0`. The instruction at PC is
`b9401909` (`ldr w9, [x8, #0x18]`). The preceding instruction loads x8 through x8;
the subsequent loop advances x8 by 8 and branches back. This is a guest pointer
walk, not an instruction that itself consumes an IPC response.

Walking backwards from that point:

| Time (s) | Service / interface | Command or activity |
| --- | --- | --- |
| 10.471009, 10.466816 | HID frontend | `SetPollingMode` (internal log, not an IPC command name) |
| 10.462240 | set:sys | 187 `GetTouchScreenMode`, returns mode 1 |
| 10.432318 | aoc | 50 `CheckAddOnContentMountStatus` (stub, no output object) |
| 10.431709 | AM application functions | 123 `GetPreviousProgramIndex` (stub, writes scalar index) |
| 10.428177 | AM application functions | 1 `PopLaunchParameter`, kind 2 (preselected user); creates `IStorage`, errors if empty |
| 10.427345 | acc:u0 | 160 `InitializeApplicationInfoV2` (stub, result-only command) |
| 10.357209–10.359039 | NVDRV GPU | `SetErrorNotifier`, `ChannelSetTimeout`, `ChannelSetTimeslice` |
| 10.243601 | VI | `GetDisplayVsyncEvent`, reports `created=true` |
| 10.231578 | AM self controller | 44 `CreateManagedDisplaySeparableLayer` |
| 10.222967–10.224133 | VI application display | 102 / 101 / 100: manager / system / relay interfaces |
| 10.208552–10.218405 | NVDRV | initialize / set ARUID / firmware memory margin |
| 10.182820, 10.183891 | NS control data | 1 `GetApplicationDesiredLanguage`, supported `0000FBFF` |

There is no terminal backtrace in this capture. Mapping the fault context to the
loader's module list gives PC **main+0x27cefb4**, LR **main+0x5f5770**.

Loaded NSOs (end addresses exclusive):

| Module | Base | End |
| --- | --- | --- |
| rtld | 0x80792000 | 0x80798000 |
| main | 0x80798000 | 0x84277000 |
| subsdk0 | 0x84277000 | 0x848e3000 |
| sdk | 0x848e3000 | 0x85538000 |

Earlier, line 261 at 9.928278 s reports `ROMFS_CHECK FAIL`, 2813 checked blocks,
295 failures, zero read errors, valid FS header hash, first bad section offset
`0xbb39c000`. For this first block, the authenticated expected hash differs from
the actual hash; raw retry, unmerged BKTR and the reference reader agree on the
same wrong bytes. Its relocation resolves to **base** offset `0xbafe2da0`.
The update is selected as `0xb0000`, with base
`dd38de587cb690a36bf4f8c5afff81dc.nca` and update
`a83558333ead6da3e4af5c8d04013bb7.nca`, no LayeredFS replacement.

## Animal Crossing: New Horizons, 01006F8002326000, update 2228224

The first read is at **16.833208 s**, line 377 (`0x8`), then `0x10`, `0x28`,
`0x18`, and execution at zero at 16.833486 s. The first fault context is PC
`0x838adc04`, `x0=0`; instruction `f9400408` is `ldr x8, [x0, #8]`.
Immediately before this, a guest `bl` at `0x838adbf0` returns a null x0, which
the caller stores and dereferences. Without that callee's code/symbols this does
not identify a service or even prove that the callee performs IPC.

Walking backwards:

| Time (s) | Service / interface | Command or activity |
| --- | --- | --- |
| 16.796504, 16.795558 | NS control data | 1 `GetApplicationDesiredLanguage`, supported `00007BFF` |
| 16.756630 | AM self controller | 14 `SetRestartMessageEnabled`, true |
| 16.756484 | AM self controller | 12 `SetPerformanceModeChangedNotification`, true |
| 16.756055 | AM self controller | 11 `SetOperationModeChangedNotification`, true |
| 16.743773 | HID | 1004 `SetTouchScreenResolution`, 1280 x 720 |
| 16.737781, 16.733572 | HID frontend | `SetPollingMode` |
| 16.728101 | set:sys | 187 `GetTouchScreenMode`, mode 1 |
| 16.630915–16.632571 | NVDRV GPU | same notifier / timeout / timeslice activity as TotK |
| 16.518837 | VI | `GetDisplayVsyncEvent`, `created=true` |
| 16.506745 | AM self controller | 44 `CreateManagedDisplaySeparableLayer` |
| 16.497410–16.499377 | VI application display | 102 / 101 / 100 manager / system / relay |
| 16.480330–16.492697 | NVDRV | initialize / ARUID / firmware memory margin |

Loaded modules:

| Module | Base | End |
| --- | --- | --- |
| rtld | 0x800fc000 | 0x80102000 |
| main | 0x80102000 | 0x85b08000 |
| subsdk0 | 0x85b08000 | 0x86174000 |
| sdk | 0x86174000 | 0x86dc7000 |

The backtrace labels every frame `unknown`. Its `Original Address` column, rather
than the synthetic `0x71...` display address, maps as follows:

| Original address | Actual module offset |
| --- | --- |
| 0x4 | unmapped |
| 0x838ad8cc | main+0x37ab8cc |
| 0x838adc48 | main+0x37abc48 |
| 0x83c1453c | main+0x3b1253c |
| 0x806c6704 | main+0x5c4704 |
| 0x83c13a1c | main+0x3b11a1c |
| 0x838c0114 | main+0x37be114 |
| 0x838e538c | main+0x37e338c |
| 0x838e442c | main+0x37e242c |
| 0x862b5c30 | sdk+0x141c30 |
| 0x862bc3b4 | sdk+0x1483b4 |
| 0x0 | unmapped |

First-fault PC is **main+0x37abc04**, LR **main+0x37abbf4**.

Earlier, line 257 at 16.390804 s reports `ROMFS_CHECK FAIL`, 2021 checked blocks,
72 failures, zero read errors, valid FS header hash, first bad section offset
`0x1cfbd8000`. Again the detailed mismatches are base-sourced, and raw/unmerged/
reference readers agree on bytes that fail the expected IVFC hash. The selected
update is `0x220000`, base `bd68bf29c8500e9c88ca844e1a6f77e8.nca`, update
`e10617820db06889e1638499478da0de.nca`, no LayeredFS replacement.

## UWP comparison and limits of the evidence

Neither capture reports an unknown/unimplemented IPC dispatch or a missing service
before the first fault. There are no pre-fault NIFM, friends, OLSC or BCAT warnings.
This does not prove that all calls succeeded: debug-level calls and IPC payloads
are absent. SVC history shows `SendSyncRequest` (0x21) returning kernel success,
which says nothing about the CMIF result inside the reply.

Source inspection of `NXBOX_UWP` / `YUZU_UWP_APPCONTAINER` branches found:

- The listed AM/NS/AOC/SET/NVDRV handlers use shared desktop implementations.
  VI manager/system stubs allocate interfaces, relay obtains the binder driver,
  and managed display creation delegates to the display layer manager. The word
  `STUBBED` alone is not evidence of a null returned object.
- Account differences concern default profile/avatar, multi-account policy and
  default fake Nintendo-account linking (`acc/offline_account.h`). They do not
  change `InitializeApplicationInfoV2` or the successful preselected launch-data
  allocation. Both captures report a prepared user in `diag.txt`.
- UWP applet policy prefers local firmware and can recover through HLE; the error
  applet defaults to HLE. No applet creation/recovery/error request appears in the
  fault window. These branches do not run in the listed self-controller calls.
- UWP VFS uses FromApp enumeration/open/stat APIs. The read path still locks the
  file reference across a 64-bit seek and fread. Save-data paths are not disabled
  by these guards. The core target includes the service implementations; it does
  not substitute empty NIFM/friends/BCAT/OLSC servers under these macros.
- Shared system fonts are successfully loaded from NAND in both logs. This is
  evidence for those archives, not proof that every firmware archive is present.
- The reference RomFS reader independently checks relocation and patch CTR, but
  **shares original/base storage decryption** with production. Agreement cannot
  exonerate base decryption, underlying file I/O or the actual bytes on the Xbox.
  It also cannot establish a bad dump when desktop reportedly boots it.

## Implemented correction and next decisive evidence

`src/core/hle/service/filesystem/fsp/fs_i_storage.cpp` previously ignored the
backend read count and unconditionally returned success. It also passed an
untrusted length to the backend without bounding it by the output IPC buffer.
It now validates the buffer and storage range without overflowing offset+length,
handles empty reads, and returns an error with offset/requested/actual diagnostics
on incomplete reads. This applies equally to desktop and Xbox. It does not repair
full-length reads containing wrong data or verify IVFC during guest reads.

`tests/port/test_fsp_storage_read.py` compiles the actual production method against
a controlled backend. It exercises empty/partial/full reads, buffer canaries,
negative arguments, EOF, extreme offsets, and offsets beyond 4 GiB.

**No per-game causal fix is established yet; no shared root cause is claimed.**
The next useful comparison is the same selected base/update NCA and failing IVFC
block on desktop and Xbox, plus a boot trace with `NXBOX_SERVICE_TRACE=1` and FS
debug logging. Trace results distinguish CMIF errors from transport success;
compare the guest callee at ACNH `main+0x37abbf0` and TotK's pointer source if data
integrity agrees. Existing service tracing deduplicates calls, so it is a command/
result inventory rather than a complete chronological IPC history. A successful
Xbox boot after this correction is required before calling either game fixed.

## Validation

- `python3 -m unittest discover -s tests/port`: 888 tests in 377.479 s,
  44 skipped, two environmental import errors (`fontTools`). This discovery began
  before the new test file was created.
- The new test was run separately with
  `python3 -m unittest discover -s tests/port -p test_fsp_storage_read.py`: passed.
  A negative control compiled the same harness against the original HEAD handler
  and rejected its success on an empty backend read.
- Ruff check/format, clang-format verification for the changed C++ file, and
  `git diff --check`: passed.
- No Xbox build, runtime confirmation, commit, or modification of evidence files.
