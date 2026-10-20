# DBSZ submission pressure (2026-10-20)

Evidence: read-only `compat-2026-10-18/w-dbsz/diag.txt`. No console run or
performance measurement of the changes was possible.

## Diagnosis

Line 3420 has **226 frames**, not 110: normalize that window by 226. The run's
22 fps summary is not the rate of this particular window.

| Work in that window | Count | Per frame |
| --- | ---: | ---: |
| ExecuteCommandLists | 10,544 | 46.65 |
| Guest fence submissions | 9,455 | 41.84 |
| Buffer download submissions | 851 | 3.77 |
| Clear submissions | 12 | 0.053 |
| Present submissions | 226 | 1 |
| Query readback / bank, texture download, staging pressure | 0 | 0 |

`InnerFence::Queue()` submits every non-stubbed guest fence by default. Shared
`SignalFence()` also calls `FlushCommands()`; the second empty submit is elided,
but this still closes a real recording almost every guest fence. The log explicitly
has `NXBOX_D3D12_FENCE_BATCHING=0` (line 48).

`waits_top=finish:856:250.675` locates nearly all the 251.996 ms wait time in
`Context::Finish()`, not allocator or timestamp-slot reuse. Buffer cache CPU
reads call `DownloadBufferMemory -> runtime.Finish -> Submit/WaitSubmitted` before
copying mapped bytes into guest RAM. The same runtime Finish was also used by
`DMACopy` under balanced/accurate fence settings; the old counters cannot separate
those callers. Download staging traffic is 1,256,800,640 bytes, including async
flush snapshots; this is not evidence that every download can be skipped.

The recorder's 728 backpressure waits consume **0.902 ms/frame**. They are CPU
chunk-pool waits, not GPU fences, and each partial submit occupies a chunk. The
old eight-chunk pool can fill while the recorder executes many tiny lists (or
waits to recycle one of 16 native allocators). Growing staging would not fix this
window: staging-pressure submissions are zero. Query/texture paths are not its
fence-wait source either.

## Changes and expected savings

- Default recorder capacity: 8 to 32 chunks (2 to 8 MiB, plus retained object
  references). Order, ownership, bounded backpressure and failure draining stay
  intact. Expected benefit is burst absorption; **0–0.902 ms/frame** is the
  observed wait opportunity, not a promised saving or a throughput cure.
- Default clears no longer Finish after each MiB. The existing 64 MiB staging
  pressure path still submits/waits before recycling pages. A 3 MiB + 12-byte
  clear produces identical bytes with zero intermediate finishes instead of
  three. In the cited steady window, only 0.053 submissions/frame are affected:
  about **0.0015 ms/frame** using its average submit cost, plus an unisolated
  portion of fence time. Larger clears can benefit more.
- `NXBOX_D3D12_ASYNC_DMA=1` defers the buffer DMA-only Finish. Dirty-range tracking,
  queue order, resource retention and mandatory CPU readback waits remain in
  place. Default timing remains synchronous. New wait origins `dma_copy` and
  `buffer_download` distinguish the two callers on the next run. Its saving is
  unknown until that split is measured; **1.115 ms/frame** is the upper bound
  represented by *all* current fence waits, not a DMA prediction.
- Reuse the **existing**, opt-in `NXBOX_D3D12_FENCE_BATCHING=1` for the dominant
  submit reduction. Expanded production fence-manager tests cover 42 syncpoints
  sharing one tick, ordered callbacks only after GPU completion, forced drains,
  failure and shutdown. DMA-memory-sync, present, normal flush and forced-drain
  hooks already submit deferred ticks. No fence-batching implementation was
  added here. Removing most guest-fence submits offers roughly **1.18 ms/frame**
  of submit time (89.7% of 298.019/226), assuming similar submit cost; actual batch
  boundaries and guest dependencies determine the saving.

These opportunities overlap and must not be added blindly. None demonstrates
60 fps on Series S. CPU-required reads remain synchronous: returning stale guest
bytes would change output.

## Early hitch

The file records **47,765 ms** between presentations (line 1266), rather than a
single logged 46-second renderer operation. Three frames have appeared by
`tick_ms=41600703` (line 433); presentation stays at zero through
`41645734` (line 1172), then resumes before `41650734`. Logging continues every
five seconds. GPU and D3D12 recorder samples are 0% in the intervening windows;
shader/readback/query stalls are zero. The intervening GAME_STALL windows show
about 11.492 s aggregate JIT time (9.169 s emission, nested within JIT) and 3.709 s
I/O. Guest worker threads remain active. This is guest initialization/loading
with ongoing compilation and I/O, not a 46-second D3D12 fence or PSO compile.
These aggregates do not identify one guest function or account for the entire
wall gap. There is no supported renderer hitch fix to apply from this evidence.

## A/B and validation

Use the same title, save, input sequence, cache warmth, resolution and profile
flags (`NXBOX_D3D12_FRAME_PROFILE=1`, `NXBOX_THREAD_PROFILE=1`). Compare matching
steady gameplay, separate from the loading interval.

1. Old-policy control: `NXBOX_D3D12_SMALL_RECORDER_QUEUE=1`,
   `NXBOX_D3D12_SYNC_BUFFER_CLEARS=1`; batching and async DMA unset/0.
2. New defaults: unset those two controls. Compare backpressure time/count,
   clear submissions, memory and output.
3. Enable `NXBOX_D3D12_FENCE_BATCHING=1` alone; compare total and guest-fence
   submissions/frame, submit time, syncpoint latency and output.
4. Independently enable `NXBOX_D3D12_ASYNC_DMA=1`, then combine after validation.
   Compare `wait_dma_copy_*`, `wait_buffer_download_*`, guest visibility and output.
   Disabling it restores the old DMA wait.

Host tests execute production transport, clear, DMA-wait and fence-manager code
with simulated D3D12/guest memory. They check bytes and completion ordering, not
console performance or real D3D12 driver behavior. Run:
`python3 -m unittest discover -s tests/port`.
