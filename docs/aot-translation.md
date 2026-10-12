# ARM64 ahead-of-time translation: T24

## Status and boundary

Base: `ea3244c9c`. Branch: `team/t24-aot`.

This change delivers bounded static discovery, input adapters, precompile orchestration,
and an opt-in boot integration. **It does not deliver working Dynarmic AOT yet.**
The existing A64 interface exposes Run/Step but no compile-without-execution API.
That implementation is owned by T19 (`src/dynarmic/**`) and was not modified.
Calling Run/Step to warm blocks would execute guest instructions and services and is
not a safe substitute. With the current backend, `compiled=0`, `restored=0`, and
`unsupported=discovered`; runtime JIT remains the implementation used by gameplay.

Set `NXBOX_AOT=1` before launch. Other values and an unset variable leave the existing
path unchanged. Debugger sessions and processes outside the persistent-cache title
are excluded. At the first RunThread on each core, the integration snapshots readable
executable and read-only mappings in the code region, after decompression, patching
and loader relocation. It does not scan the entire ASLR region as executable code.
Each core owns a separate JIT and therefore needs separate host-code preparation.
Transient snapshots are capped at 64 MiB per invocation, discovery at 262144 block
starts and four million scanned candidates / traversed instructions. A truncated
scan is reported; it must never be advertised as whole-executable coverage.

## Discovery

`src/core/arm/aot/discovery.h` follows B/BL, B.cond, CBZ/CBNZ and TBZ/TBNZ, including
negative displacements, call continuations and conditional fallthroughs. RET, BR,
BLR, SVC, BRK and UDF delimit traversal. BR/BLR targets require additional seeds;
PLT bodies are traversed when reachable but unresolved imports remain indirect.
STP frame prologues, BTI c and PACIASP seed additional candidates. Aligned absolute
pointers in non-executable ranges seed relocated init arrays, vtables and absolute
jump tables. Candidates must point to aligned executable bytes. False positives
are possible: discovery authorizes translation, never execution.

`image.h` accepts little-endian AArch64 ELF64 PT_LOAD ranges, defined STT_FUNC
symtab/dynsym entries, SHT_INIT_ARRAY entries and R_AARCH64_RELATIVE addends. It also
accepts uncompressed NSO text/rodata/data, its dynsym extent and build ID, and NRO
segments/build ID. Compressed NSO (including ZBI) is explicitly rejected; production
loaders must supply final decompressed bytes. No firmware, keys, title binaries or
console credentials are included in fixtures. All fixtures are generated in memory.

The runtime mapping adapter currently lacks module-specific symbol metadata and
module build IDs. ELF build-ID notes are not parsed. Relative/computed jump tables,
symbol-based relocations, stripped leaf functions without detectable prologues,
authenticated indirect branches and late-loaded modules are incomplete. ASLR-sensitive
absolute descriptors cannot be reused across relocated loads without a backend
relocation scheme. Whole-program static coverage cannot be guaranteed in general.
The runtime JIT must remain available for all these cases and for generated code.

## Required T19 hook (outside T24 ownership)

Add a public A64 method with this contract:

```cpp
// 0 = restored, 1 = compiled, 2 = unsupported.
// No guest execution, no architectural state changes, not callable during Run.
int Jit::PrecompileBlock(std::uint64_t pc);
```

T24's template adapter detects this method; its absence returns Unsupported.
T19 should use the same `Impl::GetBlock` translation/restore path, with a location
descriptor constructed from the requested PC and actual FPCR/PSTATE configuration,
not a guessed PC-as-descriptor. Keep the disk configuration key, W^X transitions,
restore guards and terminal relocation behavior. Capture the block's real provenance
(restored/compiled/not-persistable), not a guess based on a cache lookup. Do not return
Compiled if a policy or code-cache limit rejected the block. Handle code cache capacity
so preparation does not repeatedly evict the blocks it just prepared.

Add a compilation observer carrying PC, descriptor, phase (preparation/runtime),
provenance and persistable host-byte count. Count compilation events in GetBlock;
MemoryReadCode and MemoryReadCodeFresh are not compilation counters. Feed runtime
compile events to `Statistics::RuntimeCompile`. Report these separate metrics:

- Static candidates and successfully prepared/restored blocks.
- Runtime compilation count / total runtime block acquisitions (translation miss rate).
- Runtime compiled PCs absent from static discovery (discovery miss rate).
- Host bytes emitted, persisted bytes, restore failures and capacity rejection/eviction.
- Discovery, preparation, restore and total boot wall time.

Current logs explicitly show `runtime_miss_rate=unavailable`; the counters exposed by
the T24 contract are tested but not connected to the current backend. Existing
`JIT_CACHE` logs report stored bytes and blocks. Do not equate its misses with a runtime
compilation count: Find and Result can both increment misses.

## Required loader/memory hooks (outside T24 ownership)

Supply a module inventory after final patching/relocation: module build ID, load base,
executable and data ranges, symbol/dynsym roots, init-array targets and relocation-derived
roots. Re-run discovery/preparation when RO loads a new module. Combine **all** module
build IDs and patched content identity with the exact backend configuration. The existing
persistent title cache remains authoritative; T24 introduces no alternative executable
cache or configuration key. No pack-side step is shipped because host ARM64/x86 feature
and runtime configuration differences would require a target-compatible compiler.

Observe writes to executable pages in the memory subsystem, including page-table/fastmem
stores, DMA/kernel/service writes, exclusives and permission transitions. Invalidate on
all cores, flush callback instruction-page snapshots and remove/mark persistent candidates
before permitting reuse. A callback-only MemoryWrite change would miss fastmem stores and
is insufficient. Existing ICACHE invalidation and persisted-block guest-byte guards stay
in place; they are not proof that write-time invalidation is complete. Track invalidated
blocks separately from never-discovered ones; generated code stays on runtime JIT.

## Measurement and validation

Before this track: no T24 test or static-discovery pipeline. Full baseline port suite:
326 tests, four skipped, process exit code 0.

The deterministic branch fixture has ten expected block starts. Entry-only seed coverage
is 1/10 (10%); static discovery is 10/10 (100%), with 11 visited instructions and two
unresolved indirect edges. These are **fixture discovery numbers**, not source line
coverage, title coverage, gameplay coverage or performance gains.

The synthetic NSO has two expected starts. A first-process synthetic translator stores
2/2 records; a second process restores 2/2, compiling zero synthetic records. This uses
the real persistent Store and guest-byte validation, but deliberately substitutes a
synthetic translator: it does not test real Dynarmic x86 emission or demonstrate a
console cache hit. Other tests cover ELF symbols/init arrays, NRO parsing, malformed
headers, compression rejection, alignment, signed branch targets, budgets, identity
changes and an absent backend hook.

Run:

```sh
cd tests/port
python3 -m unittest discover -s . -p 'test_t24_*.py'
python3 -m unittest discover -s . -p 'test_*.py'
```

All compiled test artifacts are temporary under /tmp and removed by the harness.

## Realistic gains and console acceptance

With the current absent backend hook, expected translation-stall improvement is **zero**;
opt-in discovery adds boot CPU work. After the hooks land, AOT can move supported cold
compilations from gameplay into boot and reuse compatible persisted bodies on later boots.
It cannot eliminate shader compilation, I/O, service latency, host-code relocation costs,
FPCR variants, indirect/generated code misses or capacity eviction. The existing 16 MiB
per-title disk limit and 128 MiB per-core host cache can prevent preparing an entire title.
Do not raise those budgets without measuring Xbox process memory.

On Xbox Series X, compare AOT off/on with empty and warm disk caches over the same route,
then a route not previously played. Record boot time, actual runtime compile counts,
95th/99th percentile frame time, stored/host-cache bytes and process memory. Repeat with
changed executable bytes, module versions, CPU accuracy, FPCR, ASLR base and late-loaded
modules; verify rejection/fallback. Exercise code-page writes and invalidation through
fastmem, callbacks and kernel writes. The real backend, all-core invalidation, executable
cache guards and MSVC/UWP W^X path need console testing. Never report zero stalls from
synthetic storage tests.
