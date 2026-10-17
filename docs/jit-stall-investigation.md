# P5R JIT stall investigation

The supplied capture does not establish continuous steady-state recompilation.
`~/.local/share/nxbox/artifacts/stall-0.3.184/eden_uwp_diag.txt` contains 79 windows,
128,826 compilation calls and 67,075 ms of JIT host time (0.521 ms/call overall).
23 windows have no compilation; the last 20 contain only 760 calls / 889 ms,
with at most 139 calls in one window. The three example triples in the task are
absent from this file. They may describe another capture. No runtime engine
(Unity, IL2CPP, Mono, etc.) is assumed from the game's name.

Findings, ranked by evidence and diagnostic priority:

1. **Whole-region W^X is real; its share of compile time is unmeasured.**
   `src/dynarmic/src/dynarmic/backend/x64/a64_emit_x64.cpp:80` and `:166` switch
   protection for each emitted block. `block_of_code.cpp:227` and `:237` protect
   all committed bytes, initially committed in a 16 MiB chunk, rather than just
   the emitted block. Invalidation also switches protections, including twice
   per unpatched descriptor to call the generated dispatch lookup
   (`a64_emit_x64.cpp:797`). This explains a possible cost multiplier, not extra
   compilation calls. There is one RW/RX allocation, not a separate writable view.
   Protecting only newly emitted pages is unsafe without tracking old incoming
   patch sites (`emit_x64.cpp:367`); that redesign is intentionally deferred.
2. **Invalidation-driven recompilation remains unmeasured.**
   `a64_interface.cpp:259` starts the JIT timer only after the authoritative block
   map misses. `jitflush` counts only the less-than-1-MiB capacity evacuation;
   explicit clears and range removals previously had no counters
   (`a64_interface.cpp:112`, `:121`, `:311`). The page-table helper broadcasts to
   each CPU, but its callers are code unmapping, executable permission changes,
   and debug writes (`src/core/hle/kernel/k_page_table_base.cpp:1349`, `:2087`,
   `:3332`), not every ordinary MapMemory/UnmapMemory or IPC copy. Guest IC
   instructions have a separate route (`src/core/arm/dynarmic/arm_dynarmic_64.cpp:91`).
   Data-cache callbacks are disabled by default (`src/dynarmic/src/dynarmic/interface/A64/config.h:234`).
3. **Capacity thrashing is not supported by this capture.**
   UWP uses 128 MiB per JIT versus desktop's 512 MiB
   (`src/core/arm/dynarmic/arm_dynarmic_64.cpp:253`); all 79 `jitflush` counts are
   zero. Range invalidations remove descriptors without reclaiming emitted code;
   only a whole-cache clear rewinds the allocation. The budget is unchanged.
4. **No lookup-key defect or UWP optimization disablement was found.**
   Fast-dispatch misses still consult the block map (`emit_x64.cpp:55`). Its key
   contains PC, masked FPCR and single-step state, not FPSR
   (`src/dynarmic/src/dynarmic/frontend/A64/a64_location_descriptor.h:31`,
   `src/dynarmic/src/dynarmic/backend/x64/a64_jitstate.h:77`). Normal Run does not
   introduce single-step state. `MemoryReadCode` supplies instruction bytes,
   independently of fastmem. UWP forces fastmem off (`src/common/settings.cpp:182`)
   but retains page-table accesses and the Auto optimization path
   (`arm_dynarmic_64.cpp:336`, `:351`); safe passes default on and run through
   `src/dynarmic/src/dynarmic/ir/opt_passes.cpp:1464`. Settings overrides/debugging
   can still change this. Disabling passes or changing FP semantics is not justified.

The behavioral fix is an early return for an empty descriptor set in
`src/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp:420`. It avoids two whole-cache
protection calls when there is nothing to unpatch or remove. Live invalidations,
RSB handling and guest instruction-cache behavior are unchanged. A misleading
comment about a separate writable view was also corrected.

The next console run prints these fields on `GAME_STALL`
(`src/eden_uwp/game_session.cpp:609`; storage in `src/common/nxbox_stall.h:100`):

| Fields | Meaning / confirming observation |
| --- | --- |
| `jit_inv_calls`, `jit_inv_bytes` | A64 range requests and requested bytes, summed before coalescing and across cores. High requests alone do not prove eviction. |
| `jit_range_blocks`, `jit_inv_empty` | Actual descriptors removed by the shared x64 emitter; empty descriptor sets skipped by the fix. High removals plus repeated keys support invalidation churn. Historical range entries can yield nonempty sets with no live blocks, so empty counts are not all ineffective requests. |
| `jit_ic`, `jit_pt` | A64 IC callbacks (including ignored operations), and page-table invalidations forwarded to CPU interfaces. These distinguish guest cache maintenance from kernel code-mapping/debug-write routes. |
| `jit_clear_req`, `jit_clears`, `jit_clear_blocks` | Explicit A64 clear requests, executed A64 clears, and descriptors removed by shared x64 full clears. Requests can coalesce. Clears with `jitflush=0` reveal non-capacity flushing. |
| `jit_cache0` ... `jit_cache3` | Latest used/capacity bytes per processor ID, including prelude, sampled after compilation/clear. Capacity pressure needs `jitflush>0`; a clear may hide the preceding peak fill. Multiple processes share a slot; these are not allocation totals or committed-memory readings. |
| `jit_pc_new`, `jit_pc_repeat`, `jit_pc_max` | First-seen PCs, compilations of previously seen PCs, and maximum lifetime compile count of any tracked PC observed this window. |
| `jit_key_new`, `jit_key_repeat` | First-seen full descriptors versus descriptors compiled again. PC repeats with new keys suggest FPCR/single-step variants; repeated keys plus evictions support recompilation. Repeated keys without corresponding removals/clears merit lookup investigation. Mostly new PCs supports continued code discovery. |
| `jit_pc_unknown`, `jit_key_unknown` | History saturation; absent values cannot be classified after the tracking limit. Do not treat them as new PCs/keys. |
| `jittranslate`, `jitoptimize`, `jitemit`, `jitprotect`, `jitinvalidate` | Timed total-ms/max-ms/call-count triples. High `jitprotect` relative to `jitemit` implicates W^X; high `jitoptimize` or `jittranslate` identifies another compile stage. Timings nest: do not sum them into JIT time. Protection also occurs during initialization and invalidation. |
| `jitemitinsts`, `jitemitcapture`, `jitemitterm`, `jitemitranges`, `jitemitregister`, `jitemitpatch`, `jitemitwx`, `jitemitcommit` | Sub-phases of `jitemit` (total-ms/max-ms/call-count). `jitemitinsts`: register allocation plus the per-opcode emit handlers. `jitemitcapture`: disk-cache host-body snapshot. `jitemitterm`: terminal emission (links, RSB, fast-dispatch lookups) and deferred emits. `jitemitranges`: guest-range interval map insertion. `jitemitregister`: `RegisterBlock` (block map insert plus `Patch`). `jitemitpatch`: incoming-link `Patch()` fixups, nested inside `jitemitregister`. `jitemitwx`: `EnableWriting` plus `DisableWriting`, which contain `jitprotect` and the instruction-cache flush. `jitemitcommit`: `VirtualAllocFromApp` commit of fresh cache pages (UWP), usually nested inside `jitemitinsts` or `jitemitterm` because commits happen while code is appended. Everything except `jitemitpatch` (inside register) and `jitemitcommit` is disjoint; what `jitemit` leaves unexplained is the prologue and the cycle-count epilogue. |
| `jit_protect_bytes` | Sum of requested protection spans, including repeated flips; not memory allocated. |

Raw counters use brackets, e.g. `jit_inv_calls=[42]`, and cache gauges use
`jit_cache0=[16777216/134217728]`. This lets the existing duration-only
`stall-report.py` ignore them without misinterpreting bytes as milliseconds.
All event fields are window deltas except `jit_pc_max`; gauges persist. Independent
relaxed atomics mean adjacent-window totals can straddle a sampling boundary.
Histories are per JIT, survive cache clears, and are capped at 65,536 PCs plus
65,536 keys each. Migration to another CPU is first-seen there; histories reset
when a JIT is recreated. No successful block-lookup path gains profiling work.
Compile **every target**, including dynarmic, with `NXBOX_STALL_PROFILE=0` to
remove timers, event recording, histories and `GAME_STALL` formatting. The default
remains enabled, matching the existing profiler.

The GPU hitch is separate: capture line 71 has `gpu=6816/4898/59`, so 6.816 s is
summed work and 4.898 s the longest command. The scope measures host GPU-thread
command processing, including waits, not hardware GPU execution
(`src/video_core/gpu_thread.cpp:44`). JIT counters cannot assign that hitch's cause.

Validation: source/control-flow review, the supplied log summarizer, independent
capture aggregation, clang-format on changed lines, and `git diff --check`.
No build, emulator execution, console test, commit or push was performed. The
cause of the reported persistent high-compilation windows remains unproven until
the new identity and invalidation counters are captured in that scene.

## 0.3.220 follow-up: page-granular UWP W^X

The new console measurements supplied for this change supersede the earlier
unmeasured-cost finding: over 59 five-second windows, `jit=28403 ms`,
`jitprotect=25507 ms`, `jittranslate=273 ms`, `jitoptimize=138 ms`, and
`jitemit=27778 ms`. Protection accounts for 89.8% of measured JIT time. These
are supplied measurements, not a new capture made on this host.

### Platform decision

Use option (c), page-granular W^X. Do not assume Developer Mode grants RWX:

- Microsoft's [VirtualAllocFromApp documentation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualallocfromapp)
  explicitly rejects executable allocation protections, including RWX.
  [VirtualProtectFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp)
  permits RX with `codeGeneration`, but rejects simultaneous write and execute.
- [Nativra's CodeCache.cs](https://github.com/NspxMiguel/nativra/blob/main/x86/Nativra.X86/Jit/CodeCache.cs)
  independently documents RWX refusal measured by its console JitProbe, and
  publishes page-aligned allocations by committing RW, copying code, then
  protecting RX. [PPSSPP's MemoryUtil.cpp](https://github.com/hrydgard/ppsspp/blob/master/Common/MemoryUtil.cpp)
  also classifies UWP as W^X-exclusive.
- NXbox's `dist/nxbox/AppxManifest.xml` already declares `codeGeneration`.
  No capability change or `allowElevatedProcess` addition is made. There is no
  verified Xbox exception supporting the proposed persistent RWX allocation.
  Consequently this patch does not add an RWX probe or a misleading `rwx` log.
- Dual mapping is deferred, not declared universally impossible. Microsoft's
  [mapping creation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingfromapp)
  and [view mapping](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffilefromapp)
  documentation mentions executable access with `codeGeneration`, but does not
  establish the proposed simultaneous RW/RX alias scheme on this console.
  Dynarmic/Xbyak uses a single address for emission, RIP-relative constants,
  branch displacements, saved patch sites, dispatch/RSB pointers and Windows
  unwind offsets. Adding an alias offset only at publication would be incorrect.
  That redesign cannot be validated safely here without a target build/run.
  (Superseded: the console probe passed and the opt-in implementation is described in
  "Dual-mapped code memory" below.)

### Implementation and invariants

`BlockOfCode` now keeps the ungenerated suffix RW and non-executable. Opening an
emission only reopens its shared tail page, if any. Closing it rounds the actual
emitted span to OS pages, merges adjacent/overlapping dirty ranges, and publishes
those ranges RX. It no longer protects all committed bytes per block. Existing
code addresses and all translation/optimization/guest cache semantics are kept.

`PrepareWriteRange` opens previously published pages before writes. All four
patch forms in **both A64 and A32** call it with their exact reserved instruction
lengths, including padding. Thus `RegisterBlock -> Patch` and
`InvalidateBasicBlocks -> Unpatch -> Patch` cover old incoming branch sites,
including patches straddling page boundaries. Restoring the emission cursor does
not discard their dirty ranges. Constant-pool insertion tracks its destination
as well; updating constants is not confined to the append span. Repeated writes
to a page in the same batch do not repeat the RW transition.

The prelude ends on an OS page boundary, so block invalidation and full cache
rewinds cannot make the generated fast-dispatch lookup or dispatcher NX.
A32/A64 `Unpatch` can therefore call that lookup without closing/reopening the
batch for each descriptor. Publication happens at the end of the existing
invalidation loop. Normal `GetBlock` compiles one IR block per call, so its
existing emission scope already is one compile; batching across separate calls
would execute unpublished code. Disjoint patch/constant ranges still require
separate API calls, rather than reopening the untouched pages between them.

A full cache clear changes the retired generated-code range back to RW once,
retaining RX on the prelude, and resets the append boundary. This infrequent
operation still scales with retired cache size. Newly committed memory is added
in 16 MiB increments, with only the new suffix passed to the allocator. Memory
accounting is initialized **before** `ConstantPool`, whose constructor itself
commits memory; previously that member was initialized after those calls.
The 128 MiB cache reservation per JIT is unchanged. Prelude alignment costs at
most one page of additional padding.

Protection, commit and instruction-cache-flush failures fail closed with a
logged Windows error instead of continuing with wrong permissions. Each RX
publication calls `FlushInstructionCache`; its cost remains inside the
`jitprotect` scope, together with the protection syscall. `jit_protect_bytes`
continues counting actual protection spans. All other stall counters remain.
UWP now explicitly implies no-execute support in the target definitions even if
the CMake cache option was set off. Non-UWP protection policy is unchanged.

The fallback choice is W^X itself: there is no RWX mode to fall back from and no
attempt to bypass an allocation/protection failure. The first JIT construction
logs once per process, independent of profiling:

```text
NXBOX JIT_MODE wx protection=page-granular
```

Concurrency relies on the existing ownership contract, not a new global lock:
`KProcess::InitializeInterfaces` creates a CPU interface per core, each with its
own JIT/BlockOfCode. A single JIT's Run/Step must not overlap. Range/clear
requests take `invalidation_mutex`, request a halt, and are applied by the owning
JIT at the existing safe points. Dirty ranges, append bounds and committed-byte
accounting are per BlockOfCode. The allocator remains stateless; the page-size
and once-only log use thread-safe local-static initialization. No mutable
protection state is shared between compiling cores.

### Validation and next console run

Only source/control-flow review, clang-format on changed lines and
`git diff --check` were performed. No compilation, executable tests, emulator
run, console access, commit or push was performed. Target compilation and
runtime correctness/performance remain unverified.

On the next Windows/UWP build and console run:

1. `grep JIT_MODE eden_uwp_diag.txt` must show the `wx protection=page-granular`
   line once per process. It appears when the first JIT is constructed.
2. Repeat the same P5R scene transitions and capture `GAME_STALL` windows.
   Compare `jitprotect`, `jit_protect_bytes`, `jitemit` and `jit` against 0.3.220.
   Protected bytes should fall from repeated whole-cache spans to emitted,
   patched and constant pages. The goal is negligible protection time relative
   to the previous 25.5 seconds; **zero is not guaranteed**, because legal W^X
   syscalls and cache flushes still occur. Do not label this a measured speedup.
3. Exercise old incoming links, invalidation with FastDispatch enabled, cache
   clear/recompilation, growth beyond 16 MiB, and a 32-bit title. Check game
   behavior and absence of access violations or `NXBOX JIT ... failed` errors.
   Initialization and full clears can legitimately produce larger spans.
4. If protection still dominates despite the byte reduction, use the remaining
   call counts to distinguish fixed syscall cost from span cost before attempting
   dual mappings. This patch removes whole-cache flipping, not the W^X policy.

## Dual-mapping probe (`NXBOX_JIT_DUALMAP_PROBE=1`)

At game-session start on UWP, `src/eden_uwp/jit_dualmap_probe.cpp` maps one pagefile-backed
section as a writable view and an executable view, runs `mov eax, 0x2A; ret` through the
executable view inside SEH, rewrites the immediate through the writable view without a
protection change, and times 10,000 aliased write + `FlushInstructionCache` iterations against
10,000 `VirtualProtectFromApp` RW->RX->RW round trips. It logs every attempt plus one summary line:

`JIT_DUALMAP_PROBE create=<ok|err:code> rw=<ok|err:code> rx=<ok:mapviewoffile3|ok:mapviewoffile|err:...> exec1=<ok|fault|wrong> exec2=<ok|fault|wrong> alias_us=<total> protect_us=<total>`

`alias_us` and `protect_us` are totals over the 10,000 iterations (-1 when not measured).
Without the variable nothing runs. This is measurement only; the JIT still uses the protect path.

## Dual-mapped code memory (`NXBOX_JIT_DUALMAP=1`)

The probe above proved on the console that one pagefile-backed section can be mapped twice
(RW view + RX view), that code written through the RW view runs through the RX view, and that
a write plus `FlushInstructionCache` costs ~0.007 us against ~12.9 us for a
`VirtualProtectFromApp` round trip. The x64 backend now uses that, opt-in:

```
NXBOX_JIT_DUALMAP=1
```

Default (unset, or anything but `1`) is the page-granular W^X path described above, unchanged.
The variable is read once per process. Every `BlockOfCode` (A64 **and** A32, they share it)
logs `NXBOX JIT_WX mode=dualmap|protect reason=...` at creation: `reason=env` for dualmap;
for protect `flag_off`, `xbyak_unpatched`, `create_section_failed:<err>`, `map_rw_failed:<err>`
or `map_rx_failed:<err>`. Any failure falls back to the protect path, nothing else changes.

### Address-space invariant

**Every code address is an RX-view address; only stores use the RW view.** That covers
`getCode()`/`getCurr()`, labels, rel32/rel8 displacements, `jmp(const void*)`/`call(const void*)`,
`qword[rip + pointer]`, `putL`, block entrypoints, `patch_information` sites, the RSB and fast
dispatch tables, fastmem fault-site keys (the faulting RIP is an RX address), the Windows
unwind/function-table registration and `AllocateFromCodeSpace()` results. Offsets (`size_`,
label offsets) are view independent.

### How

- `.patch/xbyak/0001-exec-delta-dualmap.patch` (wired in `cpmfile.json`, applied by CPMUtil to the
  pinned Xbyak v7.40.1) keeps `top_` as the write pointer and adds `execDelta_` (RX - RW).
  `getCode()`/`getCurr()` return `top_ + execDelta_` (+ cursor), which makes every absolute
  computation in Xbyak use RX addresses: `opJmpAbs` (`jmp`/`call` to a pointer), RIP-relative
  `ptr[rip + pointer]`, `align()`, `Label::getAddress()`, and `LabelManager`'s absolute-label
  mode. `putL_inner` was the one site that used `top_` directly and now uses `getCode()`. Stores
  (`db`, `rewrite`) keep using `top_`. The patch defines `XBYAK_NXBOX_EXEC_DELTA`; without it (a
  system Xbyak) dynarmic compiles unchanged and reports `reason=xbyak_unpatched`.
- `UwpXbyakAllocator::alloc` creates the section (`CreateFileMappingFromApp`,
  `PAGE_EXECUTE_READWRITE`, whole cache size, committed up front), maps the RW view
  (`MapViewOfFileFromApp(FILE_MAP_WRITE)`) and the RX view (`MapViewOfFile3FromApp`
  `PAGE_EXECUTE_READ`, else `FILE_MAP_EXECUTE|FILE_MAP_READ`), and returns the RW base as `top_`.
  `BlockOfCode::ConfigureWx()` (a member initializer that runs before the constant pool is built)
  looks the RX base up and calls `setExecDelta`.
- `EnableWriting()`, `DisableWriting()` and `PrepareWriteRange()` do no protection change.
  `DisableWriting()` calls `FlushInstructionCache` on the RX bytes appended since
  `EnableWriting()` plus the exact patched ranges (merged); it is timed under `jitprotect`.
- Stores into already-emitted code go through `BlockOfCode::Writable(rx_address)`: the constant
  pool entries, the unwind info / `RUNTIME_FUNCTION` / handler RVA fields of the Windows exception
  handler (incl. `SetCallback`, which runs after the prelude is published) and the zero fill in
  `AllocateFromCodeSpace`. Block linking (`Patch`/`Unpatch`) re-emits through Xbyak after
  `SetCodePtr(rx_site)`, i.e. through the RW view. `SpaceRemaining()` compares against
  `getCode() + maxSize_`.

### Enabling and reading it

Set the environment variable before launching the title and look for the `JIT_WX` line in
`eden_uwp_diag.txt`. Compare `jitprotect`, `jitemitwx` and `jit` per `GAME_STALL` window
against a run without the variable. Memory note: the section is committed whole, so the commit
charge is the full code cache (up to 512 MiB) instead of growing in 16 MiB steps; physical
pages are still touched on demand.

Host-side model: `tests/port/xbyak_dualmap.cpp` (driven by `tests/port/test_jit_dualmap.py`)
emits through an RW alias and checks displacements/addresses against the RX alias; on x86-64
Linux it also executes through the RX alias and re-patches a branch in place.
