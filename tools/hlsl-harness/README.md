# Offline HLSL validation

Translate captured Maxwell instructions through Eden's actual frontend, emit HLSL,
and compile with DXC to shader model 6.0 DXIL. No Xbox, GPU, Wine, or emulator boot
is required. Compilation catches HLSL syntax/type/signature errors; it does not
prove rendering correctness or native PSO/root-signature compatibility.

## Get DXC

From the repository root (Python 3.12+, git, CMake and a C++ compiler):

```sh
python3 tools/hlsl-harness/fetch_dxc.py
# Linux x86-64 release layout:
export DXC="$PWD/.cache/hlsl-harness/dxc/bin/dxc"
# macOS / native source fallback layout:
export DXC="$PWD/.cache/hlsl-harness/dxc/build/bin/dxc"
"$DXC" --version
```

The script queries Microsoft's release `v1.9.2609`, selects an asset for the OS
and architecture, verifies its published SHA-256, and preserves adjacent shared
libraries and licenses. It then compiles a compute smoke shader to real DXIL.
The release inspected on 2026-10-11 contains Windows and Linux x86-64 packages,
**no macOS package**. macOS and Linux ARM therefore fall back to a native source
build. `brew info directx-shader-compiler` returned “No available formula” on that
date; `directx-headers`, `shaderc` and `glslang` do not replace DXC.

To explicitly build DXC on macOS (Apple Silicon or Intel) or Linux:

```sh
# macOS prerequisites; this does not change Apple accounts or signing:
brew install cmake boost
python3 tools/hlsl-harness/fetch_dxc.py --source --jobs 2
export DXC="$PWD/.cache/hlsl-harness/dxc/build/bin/dxc"
```

The source build runs these exact commands (paths expanded by the script):

```sh
git clone --depth 1 --branch v1.9.2609 https://github.com/microsoft/DirectXShaderCompiler .cache/hlsl-harness/dxc/source
git -C .cache/hlsl-harness/dxc/source submodule update --init --recursive
cmake -S .cache/hlsl-harness/dxc/source -B .cache/hlsl-harness/dxc/build \
  -C .cache/hlsl-harness/dxc/source/cmake/caches/PredefinedParams.cmake \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_INCLUDE_TESTS=OFF -DHLSL_INCLUDE_TESTS=OFF \
  -DENABLE_SPIRV_CODEGEN=OFF -DLLVM_ENABLE_ZLIB=OFF
cmake --build .cache/hlsl-harness/dxc/build --target dxc --parallel 2
```

Keep `build/bin` and `build/lib` together. Source builds are substantial; the
script limits build concurrency to two by default. A Vulkan SDK installation's
native `dxc` also works: set `DXC` to its executable path.

Upstream references: [releases](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609),
[build instructions](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2609/docs/BuildingAndTestingDXC.rst).

## Build and run

The standalone CMake target needs Boost headers >=1.81 and fetches pinned fmt
10.2.1. On Linux install CMake, a C++20 compiler and recent Boost development
headers through your package manager. A local Boost tree can be selected with
`-DBOOST_ROOT=/path/to/boost_1_87_0`.

```sh
cmake -S tools/hlsl-harness -B build-hlsl-harness
cmake --build build-hlsl-harness --target hlsl-emitter --parallel 2
python3 tools/hlsl-harness/run.py                     # synthetic VS, PS, CS IR
python3 tools/hlsl-harness/run.py --dumps /path/to/shader_dump
```

Alternatively attach the target to the portable test project (off by default):

```sh
cmake -S tests/port -B build-port-tests -DNXBOX_BUILD_HLSL_HARNESS=ON
cmake --build build-port-tests --target hlsl-emitter --parallel 2
python3 tools/hlsl-harness/run.py --emitter build-port-tests/hlsl-harness/hlsl-emitter
```

The backend integration expects
`src/shader_recompiler/backend/hlsl/emit_hlsl.h` and
`Shader::Backend::HLSL::EmitHLSL(const Profile&, const RuntimeInfo&, IR::Program&, Bindings&)`.
CMake compiles that directory's `.cpp` files when present. Reconfigure/rebuild
when the backend lands. The runner and `hlsl-emitter --probe` return **77** with a
clear `SKIP` message when it is absent; missing DXC also returns 77. Failures
return 1; only a completely successful compilation run returns 0. A stale emitter
built without HLSL also reports skip, even if the header appeared afterwards.

Results remain in `build-hlsl-harness/results` (override with `--output`): one
`.hlsl`, `.dxil` and `.dxc.log` per case. DXC is invoked without a shell as
`dxc -T <vs|ps|hs|ds|gs|cs>_6_0 -E main -Fo <output> <source>`.
Failures include DXC's diagnostics and numbered source context (full source when
no line number is reported). Each stage continues after another fails. Failed
reruns remove stale DXIL; subprocesses have a configurable 60-second timeout.

## Capture coverage and limitations

Copy the entire `LocalState\shader_dump\` directory described in
[`docs/MAINTENANCE.md`](../../docs/MAINTENANCE.md). The runner consumes the pipeline
`.txt` records and `.maxwell.bin` files, checks byte lengths, uses `address` as the
snapshot base and `start` as the shader-header address, and starts graphics CFG
translation after the 80-byte program header. `_s0`/`_s1` map to VS, `_s2` to HS,
`_s3` to DS, `_s4` to GS and `_s5` to PS. VA and VB in one manifest are merged
through Eden's `MergeDualVertexPrograms`; no independent VA compilation is counted
when VB is present. The frontend refuses truncated or out-of-bounds instruction
reads. The capture is little-endian; host support is macOS/Linux little-endian.

The GL capture does **not** contain constant-buffer values, texture descriptors,
texture-bound-buffer selection, geometry passthrough masks, or a portable decoded
fixed-function runtime state. The tool uses native-resolution settings, viewport
transform enabled, float attributes/colors, triangle topology/tessellation and
fresh bindings for each standalone shader. It does not reinterpret `.key.bin`
using a potentially incompatible host ABI. Required cbuf/texture reads fail with
an explicit missing-data diagnostic. This is a compile smoke check under stated
defaults, not an exact replay of every captured pipeline. No successful shader is
substituted for a failed capture. Compute dumps are not produced by this GL probe;
compute coverage currently comes from synthetic IR.

The synthetic VS/PS exercise generic inputs, float arithmetic and position/color
outputs; CS exercises a barrier. These programs use Eden IR objects, shader-info
collection and verification rather than canned HLSL. The private host settings
and logging adapters avoid linking the complete emulator and turn assertions into
errors. To inspect verified IR even before the HLSL backend lands:

```sh
build-hlsl-harness/hlsl-emitter --synthetic vs -
build-hlsl-harness/hlsl-emitter --synthetic ps -
build-hlsl-harness/hlsl-emitter --synthetic cs -
# Stage index, base address and start come from a manifest (decimal):
build-hlsl-harness/hlsl-emitter --dump capture.maxwell.bin 1 4096 4096 -
```

## Host regression

```sh
cd tests/port
python3 -m unittest test_hlsl_harness -v
```

The DXC integration test runs the synthetic suite when DXC and the backend are
available, building a temporary emitter if needed. `NXBOX_HLSL_EMITTER` can reuse
an existing build; `BOOST_ROOT` can supply headers. Otherwise it skips with the
missing dependency named. Driver tests check malformed captures, dual-vertex
pairing and diagnostic source context independently of backend availability.

Current checkout validation: the standalone emitter builds on Apple Silicon;
all three frontend CTest cases and a minimal Maxwell header/EXIT capture passed.
The `tests/port` opt-in CMake configuration also succeeds. Native DXC source
configuration succeeded, but compilation was stopped because the shared Mac was
at load 30 with almost all swap occupied; that source build and end-to-end
EmitHLSL/DXC compilation are not yet validated. The HLSL backend is absent, so
the integration test currently skips explicitly.
