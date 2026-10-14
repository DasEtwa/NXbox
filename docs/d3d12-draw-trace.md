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
