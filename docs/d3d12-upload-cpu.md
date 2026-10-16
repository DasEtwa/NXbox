# D3D12 index and buffer upload CPU work

## Changes and expected savings

- Index validation now uses the current buffer-cache resource identity and write
  generation plus the mapped device address (including remaps within one buffer).
  CPU dirtiness triggers synchronization before lookup; same-state
  GPU UAV/copy writes already advance the generation. Replacement and expired
  resources fail validation. Overlapping image/query writes are checked before
  lookup and published in the original download order, then invalidate the buffer
  range; raw spans preserve the coherence previously supplied by `ReadBlock`.
  A validated hit performs no guest read, content hash,
  restart scan, conversion, or download. This targets the repeated input work on
  the measured 1,705 conversion-cache hits, rather than the 25 KB of output.
  Synchronization can add an initial index-category upload for a previously
  converted-only range; inspect the upload bytes along with the CPU improvement.
- Native 16/32-bit point/line/triangle lists with compatible provoking order scan
  once for a matching restart marker. If none exists, the cached decision binds
  the normal guest buffer. CPU/GPU writes force revalidation. This eliminates
  conversion and dedicated allocation for those misses as well as later reads.
  Marker-bearing lists, narrow formats and provoking reorder retain conversion.
  Pipelines with storage/image-buffer descriptors or enabled stream output retain
  an independent converted snapshot, avoiding new read/write resource-state aliases
  when the shared cache merges guest index and shader-output ranges.
- Contiguous misses use `MemoryManager::GetSpan`; fragmented misses reuse a scratch
  vector. Inline indices use their existing span. The second `FlushCaching` inside
  index preparation was removed: `PrepareDraw` already drains it before the cache
  locks. Expected savings are one redundant flush call per conversion-path draw,
  plus allocation/copy work on misses.
- Staging requests append into available space before collecting completions.
  Fence queries, completion locking/callbacks and page reclamation are amortized
  over page exhaustion or explicit collection. Existing alignment, pressure
  submission, resource leases and retirement checks remain intact. The unit test
  verifies 1,001 small appends without an additional collection/fence query.
- Uniform snapshot lookup now uses a bounded hash index and LRU list instead of
  sweeping up to 128 weak sources and linearly comparing every range on each
  lookup. Hits require one weak identity check. This removes a separate fixed
  per-binding cost even when the snapshot already avoids an upload.
- Mapped uniforms initialize only the alignment tail; the shared cache fills the
  declared bytes immediately. This removes a redundant write of the payload.
  Existing immutable uniform snapshot caching and dirty-range upload batching
  remain in use.

These are expected operation savings, not measured Xbox millisecond/FPS claims.
No Fermi2D, blit or texture-cache source was changed. Upload copies retain all
existing copy-hazard barriers and ordering.

## Next Xbox capture

Use the same P5R heavy scene and an MK8 race with `NXBOX_D3D12_FRAME_PROFILE=1`.
Compare steady state after warmup, normalizing window totals by `frames`.

- `index_validated_cache_hits`, `index_conversion_cache_hits` (includes validated
  native decisions), `index_native_restart_draws`, `native_index_draws`;
  `index_read_bytes`, `index_downloads`, `index_download_bytes`, and existing
  conversion reason/byte counters.
- New exclusive times: `index_read_cpu_ms` (span setup or fallback read),
  `index_hash_cpu_ms`, `index_flush_caching_cpu_ms` (the existing draw-start flush),
  `index_coherence_cpu_ms` (mapping, identity, CPU tracking/synchronization),
  `index_gpu_modified_cpu_ms` (external-cache ownership and miss-only buffer checks), and
  `index_restart_scan_cpu_ms`. Actual downloads keep `buffer_download_cpu_ms`.
- Upload categories `vertex`, `index`, `uniform`, `storage`, `texel`, `other` now
  have `*_upload_count` and `*_upload_bytes_avg`. Counts increment at the existing
  nonzero category-byte event, including callers outside the buffer-cache runtime.
  They describe logical uploads/snapshots, including alignment padding, rather
  than GPU copy commands. Existing byte counter definitions are preserved.
- `staging_requests`, `staging_fast_requests`, `staging_collections`,
  `upload_copy_commands`; `upload_allocation_cpu_ms` (ring requests and dedicated
  uniform/index allocation, including mapping when a resource is created),
  `staging_collect_cpu_ms`, `upload_transitions_cpu_ms`, `upload_recording_cpu_ms`;
  `uniform_copy_lookup_cpu_ms`, `uniform_copy_cache_hits`, `uniform_copy_cache_misses`.
  Ring counts/times cover all its users, including readbacks and texture staging.
  Copy recording/transition phases cover buffer-cache copies of all heap types.
- Existing `barriers`, `copy_hazard_barriers`, `staging_copy_bytes`,
  `gpu_buffer_copy_bytes`, forced submissions and `uniform_copy_cache_hits` remain
  useful for spotting regressions.

Timing phases are exclusive: nested allocation, collection and barrier time now
moves out of its former parent. Compare the parent plus its new children, not
`buffer_upload_cpu_ms` or `index_conversion_cpu_ms` alone. Transition scopes still
charge actual transitions to `barriers_cpu_ms`; their new phase measures the
remaining caller work. Profiling overhead is not self-timed; compare profiling
on/off runs if the added timing scopes materially affect frame rate.

## Validation and limits

Portable tests exercise packed restart scanning, invalid/truncated formats,
identity/generation invalidation, weak source lifetime, cache budgets/retained
allocations, upload accounting, staging pressure and leased-page reuse. Structural
checks cover the Windows-only wiring and the absence of reads/hashes on validated
hits. Run `python3 -m pytest tests/port -q`.

The MSVC/UWP build and rendered-output comparison require the Windows/Xbox setup.
Both games still need that capture before claiming an end-to-end performance gain.
The existing content cache retains its 64 MiB budget. Validation metadata has a
1 MiB budget and both caches cap entries at 4,096. Validation holds both source
buffers and converted allocations weakly; eviction cannot extend GPU allocation
lifetimes through metadata. Fragmented ranges deliberately retain read/hash
validation rather than trusting the first physical page's generation.
