# D3D12 one-frame draw trace

Set `NXBOX_D3D12_DRAW_TRACE_FRAME=<n>` to a positive, one-based frame number.
It uses the texture cache's `frame_tick + 1`, exactly like
`NXBOX_PASS_DUMP_FRAME=<n>`. The trace needs no pass dump directory. To correlate
raw pass outputs, also set `NXBOX_PASS_DUMP_FRAME=<n>` and `NXBOX_PASS_DUMP_DIR`.
Invalid values and zero disable tracing. Configuration is read once.

Each recorded guest draw in `PrepareDraw` emits one `Diagnostic()` message
immediately before the draw command, up to 3000 messages. Skipped draws do not
consume an index. No trace formatting occurs when the variable is unset.

The following schema is wrapped for readability; each message is a single line:

```text
D3D12_DRAW_TRACE frame=N draw=N pass=N rt0_gpu=HEX topology=N
count=N instances=N is_indexed=BOOL guest_indexed=BOOL generated_index=BOOL
num_rts=N rtv=[FMT,...] has_depth=BOOL dsv=FMT readonly_depth=BOOL
pso_num_rts=N pso_rtv=[FMT,...] pso_dsv=FMT pso_samples=N pso_sample_mask=HEX8
pso_depth_enable=N pso_depth_write=N pso_depth_func=N pso_stencil_enable=N
pso_rt0_blend=N pso_rt0_src=N pso_rt0_dst=N pso_rt0_op=N pso_rt0_write_mask=N
pso_independent_blend=N pso_alpha_to_coverage=N pso_cull=N pso_front_ccw=N
pso_depth_clip=N pso_fill=N pso_topology_type=N
viewport=[x,y,w,h,min,max] scissor=[l,t,r,b] render_area=[w,h,layers]
nx_render_area=[x,y,z,w] nx_y_direction=FLOAT nx_resolution_down_factor=FLOAT
vb0_addr_lo=HEX8 vb0_size=N vb0_stride=N ib_size=N ib_format=FMT
ps_hash=HEX16 vs_hash=HEX16
```

- `draw` starts at 1 and counts recorded draws in the selected frame.
- `pass` is the active pass dump's upcoming manifest sequence. It is zero when
  no pass dump is active; `rt0_gpu` always supplies RT0's guest GPU address
  (zero for an unbound RT0).
- `topology` is the guest Maxwell enum. `count`, `instances`, and `is_indexed`
  describe the submitted command after index conversion. `guest_indexed`
  describes the original draw. `generated_index` identifies converted/generated
  quad or quad-strip indices, including expansion of indexed quads.
- Formats and D3D12 enums are decimal numeric values. `rtv` includes null RTV
  descriptors in sparse slots (`DXGI_FORMAT_R8G8B8A8_UNORM`); `pso_rtv` preserves
  the final PSO descriptor's formats, including `UNKNOWN` slots. Boolean fields
  are `true`/`false`; native PSO `BOOL` fields are decimal integers.
- The PSO summary comes from its final creation descriptor, including disk-cache
  and advanced depth/stencil stream creation paths, rather than current guest
  registers. Root constants are decoded from the words actually submitted.
- `vb0_addr_lo` contains the low 32 bits of the host vertex buffer address.
  `ib_size` and `ib_format` are zero when this draw uses no index view.
- Shader hashes are the guest hashes used in `gfx_<hash>_<stage>.hlsl` names,
  retained through synchronous and asynchronous compilation. Absent stages use
  zero. For a merged VertexA/VertexB program, the vertex hash matches the emitted
  merged shader's dump name.

## Vertex byte trace

The same frame selector also captures the first 64 recorded non-indexed draws
with at most 64 submitted vertices. Indexed draws, including generated quad
indices, do not consume this additional cap. Each qualifying draw samples guest
stream 0 and the first other stream referenced by an enabled native attribute,
for at most two streams. When several attributes use a stream, its first enabled
attribute supplies the representative IA view; D3D12 binds one IA slot per
attribute, not per guest stream.

A small READBACK buffer is allocated before descriptor reservation. Immediately
before the draw, `CopyBufferRegion` samples up to 48 bytes from the actual bound
resource, then restores its complete draw state (including
`VERTEX_AND_CONSTANT_BUFFER` for DEFAULT vertex buffers). UPLOAD resources stay
in `GENERIC_READ`. Predication is paused around the copies and restored for the
draw. No wait or forced submission is added by the capture; ordinary allocation
pressure can still submit before state recording.

Once that submission's fence completes, `Context::AfterCompletion` emits:

```text
D3D12_VB_TRACE draw=N slot=S guest=HEX host=HEX heap=N source=cache|zero|padded guest_gpu=HEX16 ia_slot=N used=BOOL host_offset=N host_size=N
```

- `draw` matches `D3D12_DRAW_TRACE`; completion messages can appear later.
- `slot` is the Tegra stream; `ia_slot` is the representative D3D12 attribute
  binding. `used` says whether an enabled attribute consumes that stream.
- `guest_gpu` is the guest stream address plus attribute offset and the first
  element times guest stride. The first element is the submitted base vertex,
  or the base instance for an instanced attribute (zero for divisor zero).
  `guest` contains 48 bytes read with `ReadBlockUnsafe`, without flushing or
  changing the cache. GPU-written guest data can therefore differ from RAM.
- `host_offset` is relative to the backing resource: bound view offset plus the
  first element times the actual IA stride, including repacking. `host` is the
  GPU copy's bytes, not the CPU mapping of a cache/upload buffer. Hex strings
  use two lowercase digits per byte with no separators.
- `host_size` is at most 48, clipped to the remaining view extent; bytes beyond
  the view are not copied or invented. A fully out-of-view range has empty
  `host` and `host_size=0`.
- `heap` is the numeric D3D12 heap type: DEFAULT=1, UPLOAD=2, READBACK=3.
  `source=cache` identifies the geometry buffer-cache binding, `zero` the
  fallback zero buffer, and `padded` either a padded DEFAULT copy or a realigned
  UPLOAD copy.
- If stream 0 is unused, an unused IA slot's bound zero view is sampled with
  `used=false`. If every IA slot is consumed by other streams, there is no
  representative view: `ia_slot=32`, `heap=0`, and `host_size=0`.

Readback owners and snapshot metadata survive until completion; the callback
never accesses guest memory or the rasterizer. Unset/invalid/zero frame selectors
perform no vertex sampling, resource tracking, readback allocation, or byte
formatting.
