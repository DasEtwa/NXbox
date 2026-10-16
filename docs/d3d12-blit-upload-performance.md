# D3D12 Fermi2D copies and texture uploads

## Findings

`RasterizerD3D12::AccelerateSurfaceCopy` previously returned `false` for every
request. Fermi2D consequently used its software blitter, including safe guest
memory reads/writes, CPU unswizzling and destination invalidation. A GPU-written
source could therefore require readback, followed by an upload of the result.

Native `Image::Transfer` also always allocated a second staging lease and repacked
all rows. This was unnecessary when the first lease already had the SDK's row and
slice layout and a 512-byte-aligned placement offset. The existing upload staging
ring is persistently mapped and fence-retired; it can supply the copy directly.

The original `texture_upload_bytes` counter counts **staging allocation requests**,
including both the cache's initial lease and the second footprint lease. Therefore
8,294,400 bytes alone does not identify one 1920x1080 RGBA8 refresh. Read the new
logical refresh counter and address diagnostic before concluding which image or
writer is responsible. No evidence currently proves a false CPU invalidation.

## Changes and expected savings

| Change | Eligibility / preservation | Expected saving |
| --- | --- | --- |
| Accelerate Fermi2D through the texture cache and `CopyTextureRegion` | Same guest format, native uncompressed color storage, SrcCopy, positive equal-sized rectangles, supported cached single-sample 2D resources, valid actual mip/layer bounds. All other cases fall back before modifying images. Small CPU-only copies retain the shared cache heuristic; copies of at least 256 KiB may create GPU images. Existing copy helper snapshots self-copies. | Removes CPU software blitting and its readback/write/upload round trip for eligible GPU-resident surfaces. Targets the eligible portion of the measured 15.3 ms Fermi2D cost; Xbox measurement is still required. |
| Discard old contents of a completely overwritten destination | Different source/destination image IDs, no destination aliases, one mip, one layer, depth one, exact base address and full coverage. Partial copies preserve the destination. | Avoids uploading old destination contents, potentially 8,294,400 bytes per 1080p RGBA8 overwrite. |
| Copy compatible staging layouts directly | SDK row/slice layout matches exactly, absolute offset is 512-byte aligned, and the entire footprint fits the original mapped lease. Downloads and mismatched layouts retain repacking. | One `CopyTextureRegion` per subresource with no additional staging allocation or CPU row copy. A compatible 1080p RGBA8 upload avoids an 8,294,400-byte staging allocation and CPU copy. |
| Bounded repeated-upload diagnostic | Enabled by frame profiling or `NXBOX_D3D12_TEXTURE_UPLOAD_TRACE=1`; 64 entries, at most one log every five seconds. | Diagnostic cost only; separately profiled. |

The fast copy preserves stored bits, including integer and sRGB values; it does
not introduce shader sampling or format conversion. Buffer upload code,
PrepareDrawIndices and the index converter are unchanged.

## Next Xbox capture

Enable `NXBOX_D3D12_FRAME_PROFILE=1`. Capture the same Persona 5 Royal scene and a
Mario Kart 8 race, including rendered output comparisons. New profile fields:

- `fermi_blits_accelerated`, `fermi_blits_software`: accepted GPU copies versus
  software fallbacks (requests, not individual GPU commands).
- `fermi_blit_accelerated_bytes`, `fermi_blit_software_bytes`: destination rectangle
  bytes for those requests; excludes pitch padding and additional self-copy snapshots.
- `fermi_discard_upload_bytes`: logical destination refresh bytes avoided by full overwrite.
- `texture_refresh_bytes`: unswizzled logical image bytes requested by synchronous
  cache refreshes, before format conversion; separate from staging allocations.
- `texture_direct_upload_bytes`, `texture_repack_bytes`: useful native transfer
  payload routed directly versus repacked, excluding row padding.
- `texture_repack_cpu_ms`, `texture_upload_diagnostic_cpu_ms`: exclusive CPU time
  for those subphases. The existing profile phases remain exclusive, so sum the
  new subphase with its parent when comparing old totals.

Continue reading `fermi2d_cpu_ms`, `texture_upload_cpu_ms`,
`texture_upload_bytes`, `staging_copy_bytes`, texture-download time, barriers and
forced submissions. Existing allocation/byte counters keep their original
semantics; bypassed work no longer contributes to them.

`D3D12_TEXTURE_UPLOAD_REPEAT` uses the same Xbox diagnostic channel as the frame
profile and reports GPU address, unswizzled size, numeric guest
pixel format, dimensions, upload count, latest reason and GPU-modified flag for
the largest repeated image in the five-second window. A larger one-off upload
does not hide a smaller recurring image. Reasons:

- `fermi_software_write`: a tracked image was invalidated synchronously during the
  software blitter's write.
- `initial_after_fermi_software`: newly created/recreated image at a recent software
  destination address (a same/previous-frame correlation, not proof of the writer).
- `cpu_write_notification`: cache received a CPU write notification outside the
  software blitter. This alone does not prove an actual guest write; broad page
  notifications can also invalidate adjacent image data.
- `initial_or_recreated`: first refresh of an image object.
- `cache_refresh_or_remap`: subsequent refresh without a recorded CPU-write transition.

A CPU notification with `gpu_modified=true` merits examining the address/write
range; it is not sufficient evidence to suppress invalidation. This change does
not suppress CPU writes or change page tracking.

## Validation

Portable behavioral tests exercise rectangle eligibility, full-subresource
coverage, aligned and padded layouts, byte equivalence with the old row repack,
bounded repeated-upload selection and window reset. Recording contracts check
preflight ordering, preservation guards and staging retention. MSVC/UWP compilation
and rendered Persona 5 Royal/Mario Kart 8 validation require Windows/Xbox and have
not been run in this macOS worktree.

Final portable suite: `python3 -m pytest tests/port -q` — **507 passed, 48 skipped,
1724 subtests passed** (pytest supplied through the existing
`/private/tmp/nxbox-pytest-deps` dependency directory). The new transfer tests also
passed separately after the final eligibility review. `git diff --check` passed.

Two existing suite failures were corrected without implementation changes: the
GPU frontend extraction fixture now includes the profiling header used by the
extracted methods, and the generated service coverage report reflects the
existing `BSD_USA::Read` implementation.
