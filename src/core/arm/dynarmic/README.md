# Xbox JIT policy

UWP Auto uses normal desktop memory-context emission. The previous Xbox
diagnostic policy wrote the guest PC with two immediate host stores before every
memory IR operation and preserved otherwise redundant register stores across
those operations. `NXBOX_JIT_FAST_MEMORY_CONTEXT=0` restores that policy;
`NXBOX_JIT_PRECISE_MEMORY=1` also requests precise crash context. Debugger use,
non-Auto accuracy modes, and protection/watchpoint exits retain precise context
by default. A protection/watchpoint exit always retains it, even when the legacy
`NXBOX_JIT_PRECISE_MEMORY=0` switch is set.

Normal execution uses the existing desktop emitter and register elimination
rules, with unchanged guest memory results and FP/synchronization flags. Fatal
crash snapshots can report a block-level PC/register snapshot rather than the
exact faulting memory operation in Auto. The disk-cache configuration signature
already includes this setting, so diagnostic and normal blocks cannot mix.

## Expected performance

There is no new Xbox measurement for this change. It removes two host stores
per memory IR operation and enables register-store elimination across memory
operations. At 3.6 GHz, an illustrative store-throughput floor of one store per
cycle means 1 million such operations remove roughly 0.56 ms of store work;
overlap, dependencies, instruction-cache effects and eliminated register stores
make this **neither a measured saving nor a frame-time prediction**. A GPU-thread
limited frame can gain zero fps. Compare with
`NXBOX_JIT_FAST_MEMORY_CONTEXT=0` on the same gameplay and cache state.

The supplied 2026-10-18 profiles contain no guest CPU-thread busy samples and no
Odyssey profile. In their final three windows:

| Game | GPU thread CPU occupancy | Recorder busy/frame |
| --- | --- | --- |
| BotW | 84.66 / 88.44 / 91.56% | 18.858 / 18.842 / 17.067 ms |
| MK8D | 72.49 / 84.02 / 82.48% | 6.956 / 6.709 / 7.186 ms |
| Wonder | 47.16 / 57.81 / 46.56% | 4.582 / 3.958 / 4.578 ms |

These are evidence of substantial renderer CPU work, not proof of an exclusive
guest CPU bottleneck. `gpu_busy_ms` is accumulated over the profiling window;
the nested CPU buckets must not be added together. Wonder's swap interval cap
cannot be removed by JIT configuration.

## Existing knobs and constraints

- Block linking, RSB, fast dispatch and safe IR optimizations are already enabled.
  Existing `NXBOX_JIT_BLOCK_LINKING=0`, `NXBOX_JIT_RSB=0`,
  `NXBOX_JIT_FAST_DISPATCH=0`, `NXBOX_JIT_CONTEXT_ELIMINATION=0`,
  `NXBOX_JIT_CONST_PROP=0`, `NXBOX_JIT_MISC_IR=0`, `NXBOX_JIT_CODE_SPEED=0`
  and `NXBOX_JIT_DISABLE_VERIFICATION=0` remove them individually.
- A64 Auto already enables unfused FMA and ignores the global monitor. Existing
  `NXBOX_JIT_UNFUSE_FMA=0` and `NXBOX_JIT_IGNORE_GLOBAL_MONITOR=0` remove them.
  These are existing accuracy tradeoffs, not newly established safe behavior.
  Zen 2 supports native FMA; unfusing is not automatically a win on this CPU.
- Inaccurate NaNs and reduced FP error remain opt-in through Unsafe accuracy.
  Changing NaN handling or reciprocal estimates can change game results. No
  supplied profile establishes their safety in BotW, MK8D or Odyssey. Existing
  `NXBOX_JIT_INACCURATE_NAN=0` and `NXBOX_JIT_REDUCED_ERROR_FP=0` disable them.
- UWP AppContainer does not allocate a fastmem arena and
  `Settings::IsFastmemEnabled()` returns false. A JIT flag cannot create an arena
  or prove fault handling. The existing checked page-table path remains enabled;
  `NXBOX_JIT_FASTMEM=0` and `NXBOX_JIT_FASTMEM_EXCLUSIVES=0` remain available on
  builds that actually provide an arena. No unsupported host address is enabled.
- A64 UWP already uses 192 MiB/core (768 MiB for four cores), with
  `NXBOX_JIT_LARGE_CACHE=0` restoring 128 MiB/core. A32 stays at 128 MiB/core;
  null JIT stays at 8 MiB. The x64 backend evacuates its cache below 1 MiB free,
  resets RSB and clears emitter links. Explicit and range invalidation remain
  necessary for correctness. None of the supplied logs provides capacity-clear
  counts justifying another memory-budget increase; the clear policy is unchanged.

Run the focused regression with
`python3 tests/port/test_jit_memory_context_policy.py -v`.
