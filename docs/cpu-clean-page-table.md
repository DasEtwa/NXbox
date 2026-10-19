# Clean CPU JIT page table

The Xbox DRAM backing cannot use fastmem aliases (`src/common/host_memory.cpp`).
The canonical table packs pointers with block/type/mark metadata. Both Dynarmic
configurations previously selected that representation, so the x64 lookup emitted
attribute masking, a marked-bit branch, and sometimes sign extension on each
memory access. With a clean table, the existing x64 emitter uses an indexed load,
a zero test/branch, and the guest address addition; it can also reuse a scratch
register (`src/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.h`). No emitter
change or guest instruction semantic change is required.

The design follows the GPL-3.0-or-later Jadyx port's `JitPageEntry` and
`docs/xbox/xbox_internal.md`, “Page table JIT limpia (0.2.65.0)”. That port's
reported frame rates are reference evidence, not measurements of this change.

## Canonical writer inventory and ordering (M1)

Repository-wide searches of `entries`, `Store`, `MarkDebug`, `MarkRasterizerCached`,
`ZeroRegion`, and `ResizeAndClear` found these paths:

| Path | Canonical operation | Clean table operation |
| --- | --- | --- |
| `Memory::Impl::MapMemoryRegion` → `MapPages` | Store pointer, Memory type, block | `PageTable::StorePage`: clear previous JIT slot; store canonical metadata; release-publish full absolute offset |
| `UnmapRegion` → `MapPages` | `entries.ZeroRegion` (may decommit canonical storage) | Atomically clear every nonzero JIT slot first; retain JIT storage for concurrent generated readers |
| `MarkRegionDebug(true)` | `MarkDebug` | Clear JIT slot before canonical mark |
| `MarkRegionDebug(false)` | Store plain Memory | `StorePage` restores offset after canonical metadata |
| `RasterizerMarkRegionCached(true)` | `MarkRasterizerCached` | Clear JIT slot before canonical mark |
| `RasterizerMarkRegionCached(false)` | Store Memory or Unmapped if pointer is absent | `StorePage` restores only plain, unmarked Memory; otherwise remains zero |
| `ProtectRegion` | No canonical writes; changes fastmem arena protections only | No translation change, preserving current page-table semantics; guest permissions remain in KPageTable |
| `SetCurrentPageTable` / process initialization | Select process's table and arena; no metadata writes | Each process owns its own pair; each process's A32/A64 JIT captures its table |
| `PageTable::Resize`, from kernel/process `KPageTableBase::Initialize*` | Resize/reset | Clear both tables on same-size reset; recreate both on size change; readers must be stopped |
| Destruction | Free sparse reservation | Free second reservation with the same existing reader-quiescence requirement |

Other `CommitRegion` calls in Memory only prepare canonical storage for reads;
`KPageTableBase` pointer/traversal helpers read it. No additional canonical writer
was found. No new permission interpretation was added to the page-table path.

Mappings and restores publish canonical metadata before the JIT release store.
Marks, unmaps, and replacements clear the JIT slot before canonical mutation.
Generated x64 loads use the existing naturally aligned 64-bit load and x86 memory
ordering; C++ readers use acquire loads. Writer/writer serialization and backing
lifetime remain the caller's existing responsibility. A zeroing store cannot
revoke a pointer already fetched by generated code: an in-flight old translation
must remain valid until that access completes. This is also a limitation of the
canonical implementation; the new table does not add a grace-period mechanism.
The tests do not claim to prove arbitrary reclamation of backing memory safe.

## Switch, configuration, and memory (M2)

`NXBOX_CPU_CLEAN_PAGE_TABLE=0` retains the original A32/A64 packed-table config and
does not reserve or maintain a second table. Other values, including unset, enable
the table. The environment is latched on first PageTable initialization, before
mappings need publication, and logged once at JIT creation. Only configurations
without a fastmem arena select the clean table; desktop fastmem config is retained.
Enabled processes maintain the shadow table even if their JIT selects fastmem.
A32 and A64 use mask 0, no marked bit, no sign extension, and keep absolute-offset,
misalignment, cross-page, callback, and CPU-accuracy settings unchanged.

Each slot costs eight bytes. A 39-bit guest address space reserves 1 GiB of virtual
address space plus a 32 KiB eagerly allocated commitment bitmap, rather than
committing 1 GiB. A 32-bit space reserves 8 MiB plus a 256-byte bitmap. Xbox's
existing SparseMemory allocator demand-commits 64 KiB chunks: a compact contiguous
4 GiB guest mapping uses about 8 MiB of JIT entries, plus rounding and other mapped
regions. Fragmented mappings can cost more; the upper bound is the reservation.
Read faults from generated code can also commit chunks containing zero entries.

JIT chunks stay committed until reset/destruction, a deliberate departure from the
reference's `jit_entries.ZeroRegion` on unmap: decommit/memset is unsuitable while
readers use atomic loads. Commitment is therefore a high-water footprint on Xbox.
Same-size reset now explicitly clears both tables because the existing
`ResizeAndClear` does nothing when the reservation size is unchanged.

`System::Run` logs `NXBOX clean page table after boot` once per application table
with `reserved_bytes`, actual `committed_bytes`, and `bitmap_bytes`, before resuming
emulation. The new SparseMemory query includes raw JIT read faults and uses the
already resolved VirtualQuery API; it does not add a loader import or hot-path
counter. This initial log does not substitute for peak process memory measurement.

## Host evidence (M3)

- `python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'`: baseline 221 tests
  passed (76.137 s); final 221 passed (69.005 s). Durations are not a benchmark.
- `python3 -m unittest discover -s tests/port -p 'test_clean_jit_page_table.py'`: two
  tests passed. The runner compiles the production PageTable and SparseLargeVector
  with `clang++ -std=c++20 -fsanitize=address,undefined`, then `-fsanitize=thread`.
  Both binaries pass in fresh processes with switch 1 and 0. The application
  logger/assert sink is stubbed; allocation, table, and atomic logic are production.
- Coverage includes negative absolute offsets, block/type preservation, debug and
  rasterizer marks/restores, Unmapped/marked entries, same-size reset, separate
  process tables, fastmem selection, switch latching, and 20,000 publication/revoke
  cycles. A reader dereferences live backing during the race, checks published
  metadata, and requires zero after observing revocation completion. Backing stays
  alive until reader join. Source checks cover A32/A64 configuration and writer
  integration; these are not emulator-level execution tests.
- `clang++ -std=c++20 -Isrc -I/opt/homebrew/include -fsyntax-only
  src/common/page_table.cpp` and `git diff --check` pass.
- Dynarmic conformance was not run: standalone CMake configuration failed at
  `CMAKE_DEPENDENT_OPTION` (it expects embedding-project setup). The workflow's
  Catch2, oaknut, and Unicorn prerequisites are absent locally. No dependency
  downloads, CI, commits, or pushes were attempted. The lead must run the workflow's
  embedding configure/build and conformance/full-suite commands in CI.

## Console A/B handoff (M4)

No build/package run IDs exist for this uncommitted change. The lead must review,
commit, build, and package it, then give the package run ID to the tester.

For Breath of the Wild, Link's Awakening, and Super Mario Odyssey, put these lines
in the installed app's `LocalState/nxbox_env.txt`, retaining other fixed settings:

```ini
NXBOX_THREAD_PROFILE=1
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_CPU_CLEAN_PAGE_TABLE=0
```

Fully terminate and relaunch NXbox between switch changes; changing the file while
running does not change the latched switch. Use the same save, camera, route,
resolution, CPU accuracy, shader cache, and warm-up for each comparison. Capture
at least 120 seconds of steady gameplay per setting; run 0 → 1 → 0 to detect
cache/warm-up drift. Confirm the one-time switch marker and the after-boot table
commitment line. Record guest game FPS, each guest CPU core thread's percent of
one host core, peak process commitment, and crash/rendering differences. Compare
CPU time per guest frame as well as percent: a faster guest may consume more CPU
percent while producing more frames. Separately exercise scene changes, suspend/
resume, game restart, and rasterizer-heavy regions to stress mark/unmark churn.

Expected effect is less work per page-table guest load/store; no FPS gain is
claimed. GPU-bound scenes can show little change. Keep `draw_recording`,
`maxwell3d_methods`, `dma_pusher`, and `macro_execution` phase data to distinguish
GPU-thread limits from guest CPU improvements. Success is a reproducible reduction
in guest CPU work per frame or higher game FPS without regressions, crashes,
rendering differences, or unsafe peak memory pressure against the 5 GiB limit.
Remaining unknowns are Xbox/UWP compilation and execution, real commitment under
gameplay churn, full Dynarmic conformance, and console performance/correctness.
