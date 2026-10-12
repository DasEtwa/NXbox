# HLSL text backend

`Shader::Backend::HLSL::EmitHLSL(profile, runtime_info, program, bindings)` emits a
`main` entry point for DXC Shader Model 6.0. It consumes the optimized Maxwell IR
and collected `Shader::Info`, including the structured syntax list. It does not
call GLSL, SPIR-V, Mesa, or DXC. The renderer owns DXC compilation and capability
checks. HLSL sources are compiled into `shader_recompiler` independently of
`ENABLE_OPENGL`.

## Integration

- Vertex A/B use `vs_6_0`, fragment uses `ps_6_0`, compute uses `cs_6_0`.
  Merge dual vertex programs and run the usual IR lowering/info collection first.
- `TEXCOORD0..31` identify guest generic attributes, with vertex signed/unsigned
  input types taken from `RuntimeInfo::generic_input_types`. Integer vertex input
  and integer render-target writes preserve bits with `asfloat`/`asint`/`asuint`.
  Fragment input interpolation comes from `Info::interpolation`; missing previous
  stage components read `(0,0,0,1)`. This version declares full four-component
  generic fields. DXC validation must reject programs exceeding native stage
  signature limits; component packing is a remaining integration task.
- `Bindings::uniform_buffer` assigns CBVs in constant-buffer descriptor order.
  Each guest buffer is a 64 KiB `cbuffer` of `uint4` words, retaining HLSL's
  16-byte array stride. Loads accept dynamic guest buffer indices and byte offsets.
  Out-of-range constant-buffer loads return zero.
- `Bindings::texture` assigns SRVs to texture descriptors first, then texel-buffer
  descriptors. This deliberately follows the P3 contract, which differs from
  GLSL's texel-buffer-first declaration order. Sampler register `sN` pairs with
  texture register `tN`; depth descriptors require comparison samplers.
- UAVs share one register namespace: start at
  `bindings.storage_buffer + bindings.image`, assign storage buffers first, then
  images and image buffers. Those two counters count resources, not two separate
  D3D12 namespaces. Consecutive calls append their UAV spans. The descriptor
  allocator must use that same per-stage append order when sharing counters
  across stages. `ssboN` uses the local IR descriptor index; its register includes
  the binding offset. Dynamic texture/image arrays use `NonUniformResourceIndex`.
- Sampler registers use `bindings.hlsl_sampler`, independently of the SRV counter:
  texel buffers consume SRVs but no sampler slots. Sampler variable names retain
  the corresponding texture's SRV number. Each sampled descriptor advances the
  sampler span, including MSAA descriptors with an unused sampler slot.
- `hlsl_root_constants.h` defines the exact 64-byte CPU ABI at `b0, space1`:
  render area DWORDs 0..3, resolution down factor 4, Y direction 5, texture/image
  scaling masks 6/7, base vertex/instance 8/9, draw ID 10, reserved 11..15.
  Upload all 16 DWORDs, using `RootConstants` defaults for identity transforms.
  Populate scaling masks with local IR indices. Static vertex Y inversion and
  depth-range conversion use `RuntimeInfo` at every return.
- A failed emission throws `NotImplementedException` and leaves binding counters
  and IR instructions/use counts unchanged. Do not cache an empty shader or
  replace an unsupported program with a trivial shader.

## Coverage relative to GLSL

| Area | Implemented |
| --- | --- |
| Arithmetic | GLSL's implemented integer, FP32/FP64, logical, comparison, conversion, select, bitwise and composite emitters; carry/overflow/zero/sign pseudo-results; packed half and double bit conversions |
| FP64 | Native double arithmetic and FMA; bit-based abs, NaN tests, truncation and nearest-even rounding helpers avoid implicit FP32 narrowing |
| Control flow | Structured if/else regions, conditional break, loops/repeat, early returns; parallel phi snapshots preserve loop-carried swaps; 8192-iteration loop safety |
| Stage interface | Vertex/fragment/compute inputs and outputs, integer targets, position, front face, vertex/instance/primitive IDs, fragment depth, coverage, clip distances, optional vertex layer/viewport outputs |
| Buffers | Byte-addressed constant-buffer loads; storage/shared 8/16/32/64/128-bit loads and stores; subword writes use CAS; local memory; 64-bit guest-address lookup through storage descriptor base/size |
| Atomics | All scalar 32-bit shared/storage/image integer atomics, including signed min/max and wrapping increment/decrement via CAS; storage FP32 add and packed half2 add/min/max via CAS; storage word-pair operations use independent 32-bit atomics |
| Textures | 1D/2D/3D/cube and array resource declarations; implicit/explicit LOD, bias, depth compare, gather component/four offsets, gradients, dimensions/LOD queries, MSAA/texel loads; literal offsets; typed integer fetch bitcasts |
| Images | Typed unsigned storage images and buffer images, load/store and scalar image atomics |
| Waves | Lane ID, 32-lane partitioned ballots/votes/masks, segmented index/up/down/xor shuffle and in-bounds result, quad lane reads, swizzle add, fine/coarse derivatives |
| Synchronization | Group barrier with synchronization, group/device memory barriers, fragment discard |

Every IR opcode has a dispatch target. Existing GLSL stubs for unlowered
register/predicate/global-atomic/bound/bindless operations and scalar FP16
operations remain explicit failures. Use the existing lowering passes rather
than passing frontend IR directly to this emitter.

## Explicit gaps and limits

- Geometry and hull/domain stages reject at entry, including passthrough,
  primitive streams, patches, tessellation coordinates and invocation IDs.
  No fake geometry/tessellation shader is returned.
- True 64-bit atomics and shared 64-bit/pair exchange reject. GLSL's non-atomic
  wide-operation fallbacks are not reproduced. Native 64-bit interlocked support
  requires a newer shader model; see Microsoft's
  [SM 6.6 atomic specification](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Int64_and_Float_Atomics.html).
- Explicit comparison-sampling LOD is supported only when the IR LOD is literal
  zero, using `SampleCmpLevelZero`. Arbitrary comparison LOD requires a later
  shader model; see the [SM 6.7 announcement](https://devblogs.microsoft.com/directx/shader-model-6-7/).
  Depth bias, LOD clamps, sparse residency, dynamic sampled offsets and cube texel
  fetch reject. Integer filtered sampling rejects and requires texel fetch.
- Typeless/signed storage images, cube UAVs, constant/storage descriptor arrays,
  indexed output attributes, point size/point sprite coordinates, viewport masks,
  legacy varyings, transform feedback, alpha testing and dual-source blending
  reject. Vertex layer/viewport outputs require the device capability in Profile.
- Helper-invocation queries and W-scale-factor system registers reject. SM 6.0
  lacks `IsHelperLane`; see Microsoft's [SM 6.6 announcement](https://devblogs.microsoft.com/directx/hlsl-shader-model-6-6/).
  Demote uses terminating discard, matching GLSL's approximation.
- Wave ballots/shuffles assume host waves have at least 32 lanes. Check D3D12
  OPTIONS1 before selecting this backend. Cross-lane reads of inactive/helper
  lanes have native HLSL undefined behavior. Profile's gather subpixel workaround
  and per-instruction floating-point denormal/rounding modes are not applied.
  FP32 FMA maps to `mad`, whose fusion behavior is hardware-dependent; see
  [Microsoft's mad documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/mad).

## Host validation

The std-only root ABI/binding helper test is included in the Python port suite
and standalone CMake build. When fmt and Boost headers are available, CMake also
builds the complete emitter with Eden's real IR implementation and exercises
vertex, fragment, compute, phis/loops, wave pseudo-results, depth compare/gather,
resource ordering, image operations, repeated emission and failure rollback.

```sh
cmake -S tests/port -B /tmp/nxbox-port-tests \
  -DCMAKE_PREFIX_PATH=/path/to/fmt \
  -DHLSL_BOOST_INCLUDE_DIR=/path/to/boost-headers
cmake --build /tmp/nxbox-port-tests --parallel 2
ctest --test-dir /tmp/nxbox-port-tests --output-on-failure
cd tests/port
python3 -m unittest discover -s . -p 'test_*.py'
```

IR tests leave `hlsl_vertex.hlsl`, `hlsl_fragment.hlsl`, `hlsl_compute.hlsl`,
`hlsl_waves.hlsl`, `hlsl_arithmetic.hlsl` and `hlsl_resources.hlsl` in the build/test working directory.
If DXC is available, CTest also validates these files as DXIL. Host C++/text
checks alone are not proof of DXIL validity or rendered guest-shader correctness;
Windows/Xbox DXC and GPU integration validation remain necessary.
