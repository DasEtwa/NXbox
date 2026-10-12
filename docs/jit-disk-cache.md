# Persistent A64 to x64 translation cache

## Verdict and scope

Persistent host code is feasible, but a dump of Dynarmic's code allocation is
**not reusable across processes**. Relocation must preserve generated-code
metadata as well as instructions. A general solution needs emission-time
symbolic relocation, constant-pool capture, block-link reconstruction and
fastmem fault-site reconstruction. Guessing pointers by scanning machine code
cannot distinguish a host address from a legitimate guest integer.

This change implements an opt-in, fail-closed first version for any A64 game,
without a title-specific allowlist. Its block coverage is deliberately small:
audited integer/register operations and a simple leaf terminal. Unsupported
blocks always follow the existing translator, optimizer and emitter. This is
not yet an all-block cache, and does not establish recompiler-like level loads
for every game. A32 and non-x64 backends remain uncached.

`NXBOX_JIT_CACHE=1`, with that exact value, enables the UWP game session. It is
absent/off by default. The file is
`LocalState\jitcache\<16-digit-title-id>.bin`. A nonzero executable build ID
is required; sessions without one report `disabled_no_build_id` and translate
normally. No new backend service, scheduled task or external cache sharing is
needed.

`docs/MAINTENANCE.md` was read. The requested `docs/cpu-jit-audit.md` is absent
from the working tree, but was recovered and read with
`git show 2a82869005:docs/cpu-jit-audit.md`; commit `f8040ced1e` removed it while
reverting the timing changes it described. Its timing implementation is historical,
not an active dependency of this cache. `docs/jit-stall-investigation.md` provides
the additional JIT evidence, including page-granular W^X. Its older whole-allocation
protection findings must not be treated as the current path.

## What depends on the process or placement

Source paths below are relative to `src/dynarmic/src/dynarmic/` unless noted.

| Dependency | Actual source / representation | Required general treatment |
| --- | --- | --- |
| Host calls and callback objects | `backend/x64/block_of_code.h` `CallFunction` / `CallLambda`; `backend/x64/callback.cpp` `ArgCallback` / `SimpleCallback`; `backend/x64/devirtualize.h`; `emit_x64.cpp` `EmitCallHostFunction`. Calls can be rel32 or a register loaded with an absolute target. Devirtualized callbacks also capture an object/adjusted `this`. | Stable symbol IDs for helper/function and callback receiver; rel32 records or indirect calls through a per-JIT process table. Never save the old `this` or function address. |
| Page-table and fastmem bases | `backend/x64/a64_interface.cpp` `GenRCP` loads the current page table into R14 and fastmem base into R13. `a64_emit_x64_memory.cpp` emits accesses/fallbacks using these registers and generated fallback addresses. | Rebuild the prelude with current pointers. Relocate any embedded data/callback addresses separately; saving R13/R14 register numbers is fine, saving their values is not. |
| TLS and exclusives | `a64_emit_x64.cpp` TPIDR accessors insert `conf.tpidr_el0` / `tpidrro_el0` pointers. `a64_emit_x64_memory.cpp` exclusive paths depend on the monitor and processor ID. | Process-table entries for TLS/monitor objects; include semantic configuration in the key. |
| Block links and RSB | `a64_emit_x64.cpp` `EmitTerminalImpl`, `EmitPatchJg/Jz/Jmp/MovRcx`; `emit_x64.cpp` `PushRSBHelper`, `RegisterBlock`, `Patch`, `Unpatch`. Link bytes mutate after emission. Reserved patch sizes are 23/23/22/10 bytes. RSB entries contain real code pointers. | Store guest descriptors and patch kind/offset, not final linked bytes. Recreate `patch_information`, register each new block, and rebuild links to current entrypoints. Reset RSB/dispatch state; never serialize populated tables. |
| Fast dispatch | `a64_emit_x64.cpp` `GenTerminalHandlers` embeds the instance's table address and generates per-instance handlers/lookup code. | Rebuild handlers/tables; relocate branches to symbolic handler IDs. |
| Constant pool | `backend/x64/constant_pool.cpp` stores 16-byte constants in the code allocation. `BlockOfCode::Const` returns `frame[rip + constant_address]`. `reg_alloc.cpp` can introduce a pool reference even for an immediate loaded into an XMM register. | Capture constant values and each RIP-relative reference at emission, allocate a new pool entry, then patch disp32 with a checked range. Deduplicate by value, not old address. |
| RIP-relative branches/data | Xbyak labels inside a copied body preserve relative displacement if both ends move together. References to prelude, another block, pool or data outside that body do not. | Classify every external reference; record offset, width, symbol, addend and instruction-end base. A single allocation delta is insufficient. |
| Fastmem exception sites | `a64_emit_x64.h` `FastmemPatchInfo`; `a64_emit_x64_memory.cpp`; `a64_emit_x64.cpp` `FastmemCallback`. The map uses actual fault RIPs, resume RIPs, fallback callbacks and do-not-fastmem markers. | Serialize symbolic fault/resume offsets and recreate the exception map before publication; reset process-specific retry state. |
| Host ABI/features/unwind | `abi.h`, `stack_layout.h`, `a64_jitstate.h`, `block_of_code.cpp`, `backend/exception_handler_windows.cpp`. State offsets, Windows shadow space, supported instructions and allocation-relative runtime metadata matter. | Exact emitter/build/ABI/configuration key, current exception registration, bounded allocation and checked relocation. |

A full implementation should route host callbacks through a per-process table
owned by the JIT. The table can contain current TLS, monitor, callback receiver,
helper and prelude addresses. The table base must itself come from current
runtime state or have an explicit relocation. Internal branches remain direct;
block links keep Dynarmic's existing patch mechanism. This trades some indirect
loads/calls for much smaller and more auditable relocation coverage.

## Implemented relocation and admission

`src/common/nxbox_jit_cache.h` is a std-only codec/store/relocation layer. It does
not serialize C++ objects, addresses, padding, `size_t`, unordered containers
or raw `UserConfig` memory. Integers are explicit little-endian 64-bit values,
blobs have bounded lengths, and each record has a corruption checksum.

`A64EmitX64::DiskCacheEligible` checks the IR both **before optimization** and
at capture. Pre-optimization checking matters: a read-only-memory load folded
to a constant would otherwise hide an unhashed data dependency. The allowlist
covers scalar register/SP/PC get/set, logical operations, add/subtract and
associated carry/overflow/NZCV bookkeeping. It rejects memory, SVC, host calls,
TLS, exclusives, FP/SIMD, PushRSB, conditional/check-halt/check-bit terminals,
single stepping and every unknown opcode. Verbose-debug emission is not cached.

Capture occurs before terminal emission, after restoring RBP and adding the
cycle decrement. It rejects deferred emission and any `BlockOfCode::Const`
reference, including references introduced by register allocation. The accepted
body contains runtime-state accesses through ABI_JIT_PTR and stack-relative
spill/state accesses, with no external code/data reference.

The emission-time relocation record is a **symbolic terminal reconstruction**:
body-end offset, leaf kind, and optional target guest location descriptor. It
is not an arbitrary x64 byte-offset relocator. The stored body ends before
mutable links. On restore, `NxboxJitCache::Relocate` copies that body and the
existing leaf emitter emits a fresh terminal at its new address. This resolves
current prelude/RSB/dispatch handlers and creates current `patch_information`.
`RegisterBlock` reconnects existing incoming links. Restored guest ranges are
registered in `block_ranges`, so regular invalidation finds these blocks too.

Loaded blocks also receive a newly emitted process-local **entry guard**. Its
context, record pointer and call target are regenerated, never stored. Guest
state is materialized at the block boundary, so the guard uses the reserved
ABI call space, preserves ABI callee-save registers, and switches MXCSR out
and back around the host call. A false result takes the existing force-return
prelude with host MXCSR already restored, before the persisted body executes.
No caller-save value is live between independently compiled blocks.

The guard is registered as the block entrypoint. Direct links, RSB and fast
dispatch therefore cannot bypass it. Guard record addresses are stable in a
per-JIT `std::map`; range invalidation retains them until a full code-cache clear
retires all generated users. Host/config blob copies are freed after restore;
only guest bytes and bookkeeping are retained for execution-time guards.

## Identity and guest modification

The effective cache identity is:

1. Title ID and loaded application's 32-byte NSO build ID, obtained after
   `System::Load`, including the selected executable update.
2. Guest PC and complete Dynarmic location descriptor: PC, FPCR variant and
   single-step bit (single-step entries are rejected).
3. Every instruction word actually consumed by translation, plus its hash.
4. Exact serialized Dynarmic configuration and its effective host-feature set.
   Configuration includes optimization/unsafe flags, memory mode and bit/stride
   settings, timing, hooks, processor ID and system-register constants. Pointer
   presence is serialized; pointer values are not.
5. Emitter identity: CMake SHA-256 source/dependency-manifest/toolchain/flags
   fingerprint, build configuration, format version, state/stack layout sizes
   and ABI shadow-space size. Source edits trigger CMake reconfiguration and a
   new fingerprint. Builds without that identity cannot admit cached code.

Each CPU registers its active configuration before the session opens the file.
Opening drops records for obsolete emitter/configuration keys, reclaiming their
admission budget so a full old cache cannot prevent a new build from caching.
The record map uses exact configuration bytes, rather than trusting a
configuration hash collision. Guest FNV-1a hashes are corruption/index checks;
**byte equality is authoritative**. The in-memory and on-disk executable
payload is trusted local data, not authenticated input. This format is not
safe for downloaded/untrusted host-code caches, even if guest bytes match.

Translation records the exact callback reads and rejects inconsistent repeated
reads of the same instruction during one translation. Only complete, contiguous A64
instruction spans up to 4096 bytes are admitted. Those words are compared again
with fresh memory before recording. A restore validates fresh words before
publication, and the guard compares every word again at **every execution
entry**, without allocation. A missing mapping/read or any changed byte rejects
the cached body, sets PC to that block's guest start, requests normal range
invalidation and exits to the host. The next lookup translates normally if the
disk record still mismatches.

The existing `DynarmicCallbacks64::MemoryReadCode` keeps a page snapshot. A new
`MemoryReadCodeFresh` bypasses that snapshot through valid-range checking and
`Memory::Read32`; opted-in translation also uses this fresh path. Other
frontends default to their existing callback, and do not enable the cache.

Do not assume this checkout invalidates every ordinary write immediately.
Guest IC maintenance, executable mapping/permission changes and explicit debug
writes already route to Dynarmic invalidation; regular direct page-table writes
can bypass a callback. The entry guard covers changes before entry even when
those invalidation routes were not called. Concurrent mutation *after* the guard
has read an instruction and while the block executes still needs the normal
guest instruction-cache synchronization contract. Literal atomic validation
against arbitrary unsynchronized concurrent code writes would require locking
writers for the entire execution interval, or an instruction-by-instruction
interpreter; neither is provided here. This limitation must be part of console
self-modifying-code validation, not hidden behind a guest-code hash.

ASLR that changes a guest module's PC or absolute guest constants produces a
miss. It is safe but reduces reuse. Other module updates and executable patches
are checked by their actual block bytes even when the main build ID stays the
same. A future general cache should add each module's identity/relative PC and
all folded data dependencies rather than assuming the main NSO names everything.

## Storage, lifecycle and failure behavior

The frontend creates a shared per-title store before CPU interfaces are
constructed, opens it with final build metadata before CPUs run, then flushes
it after `Pause` / `ShutdownMainProcess`. Only matching-title, non-debug A64
interfaces with a real page table receive it; null JITs and applet processes
are excluded. Four cores share a mutex-protected store instead of overwriting
each other's files. Configuration keys still distinguish core-specific code.

Admission is bounded to 16 MiB per title. The dedicated directory is pruned to
128 MiB by removing the oldest other `.bin` files after a successful save.
The 16 MiB bound is encoded record data, not a total RAM bound: map/vector
bookkeeping, the startup file buffer, snapshot serialization and per-core guest
guard footprints add memory. Console MEM/GUEST_MEMORY measurements must include
these peaks under the existing 5 GiB process budget.
Snapshots are written to `<title>.bin.tmp`, checked and closed, then renamed.
Windows requires removing the disposable old cache before `std::filesystem`
rename. A crash in that gap loses a cache, not game data; truncated or checksum-
invalid files are discarded as a whole. There is no claim of crash-durable
atomic replacement or a filesystem authenticity guarantee. Save data paths
are untouched.

New record insertion checks for a checkpoint every 30 seconds; the timer is
opportunistic, not a worker thread. With no newly admitted blocks there is no
periodic write. Game exit persists remaining dirty records. Reaching the cap
stops admission, not emulation. Storage/I/O/codec failures are nonfatal misses.
Record sizes, file size, count, terminal enum, body-end offset, address overflow,
checksums, identity and trailing bytes are validated before installation.

`JIT_CACHE phase=load|save|exit hits=... misses=... bytes=... stored_bytes=...
blocks=...` goes through the frontend's existing `Diagnostic` path. Hits count
restored blocks, bytes count installed host bytes including fresh guards and
terminals, and misses count absent/rejected records plus execution-time guard
failures. These are cumulative session counters, not per-frame timings. Saving
is synchronous and its cost remains observable in JIT stalls; no hidden async
writer races with a core or prolongs shutdown.

The existing code-cache reservation and page-granular W^X remain in force:
commit sufficient space, make the new tail writable, copy/re-emit, register and
patch, then publish RX with the existing instruction-cache flush. No RWX mapping
or execution directly from a file is introduced.

## Expected gains and acceptance criteria

A hit skips A64 translation, IR optimization and most instruction selection;
it still reads guest words, copies host bytes, emits a guard/terminal, patches
links and performs legal W^X publication. A per-entry host call and byte check
add steady-state CPU cost. With this deliberately narrow coverage, a net gain
is **not established** and steady-state performance can regress. Keep the flag
off until target measurements support enabling it.

For a load with approximately 23,000 new blocks, useful reuse depends on the
fraction of *compile time* represented by eligible matching blocks, not only
block count. If that fraction is `f`, the ideal compile-work reduction is
`f * baseline_jit_time`; subtract I/O, validation, copying, relocation, protection
and guard execution. No universal load-time number follows from the count.
The previous P5R stall audit's expensive protection spans have since been
reduced; disk caching cannot claim those old savings a second time.

For the general version, first add auditable relocations to memory/callback
paths and external RIP constants, then conditional terminals and fault-site
metadata. Extend the key with every additional dependency. Expand admission
only after relocation and emitted-code equivalence tests pass. Removing the
per-entry guard requires a proven write-generation/invalidation protocol across
all guest writers, including direct memory accesses and kernel copies.

Required Xbox validation before any default-on proposal:

- Cold run, graceful exit, app restart and identical level entry for multiple
  unrelated A64 games, with baseline/first/second `GAME_STALL` and `JIT_CACHE`
  logs, frame progression and game correctness.
- Code patched before restore and after installation, IC range/full clear,
  incoming links/RSB/fast dispatch, FPCR variants, process recreation, config
  changes, NSO updates and guest address changes. A stale entry must exit before
  its body; invalidation and the normal compiler must recover.
- Suspend/terminate during checkpoint, corrupt/truncated files, disk-full/write
  failure, per-title/directory caps and source-version changes.
- Windows UWP compilation and runtime ABI/W^X verification. Host std-only
  tests cannot establish the guard's generated Windows ABI or console speed.

Portable regression coverage lives in `tests/port/test_jit_disk_cache.py` and
executes production serialization, relocation dispatch, allocation-free guest
checks, cross-process symbol resolution, bounded storage, corruption recovery
and concurrent insertion using only standard C++ facilities. Run the requested
suite from `tests/port`:

```sh
python3 -m unittest discover -s . -p 'test_*.py'
```

The pristine `/tmp/mesa-pin` checkout is only read by existing tests and must
not be changed for this feature.

Validation on this Mac: the exact Python discovery command passed **191 tests in
141.507 s, exit code 0**. The three changed x64 implementation units passed
AppleClang x86-64 C++20 syntax checking with the pinned, checksum-verified Xbyak
headers, CPU-feature detection enabled and stall profiling disabled. A separate
small CMake probe compiled the generated emitter identity and automatically
reconfigured/rebuilt after fingerprint inputs changed. This was not a full
Dynarmic/emulator link or a Windows UWP build. `git diff --check` passed, changed
C++ ranges/new headers used clang-format, and the Python test used ruff format.
`/tmp/mesa-pin` remained clean at the pinned revision.
