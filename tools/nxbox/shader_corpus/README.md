# Offline shader corpus

This host tool replays Eden's captured guest environments through the **real Maxwell
frontend, IR optimization passes, and EmitHLSL** without a GPU, game launch, or renderer.
Each compilation runs in a separate process. Exceptions, assertions, crashes, invalid
inputs, and timeouts become individual failure rows; processing continues.

## Build and run

Requires CMake >= 3.20, a C++20 compiler, Python >= 3.10, and Boost >= 1.81.
CMake fetches the same pinned fmt 10 revision as the existing HLSL harness. An existing
fmt checkout can be supplied with `-DFETCHCONTENT_SOURCE_DIR_FMT=/path/to/fmt`.

```sh
cmake -S tools/nxbox/shader_corpus -B /tmp/nxbox-corpus-build \
  -DBOOST_ROOT=/path/to/boost
cmake --build /tmp/nxbox-corpus-build --target nxbox-shader-corpus --parallel 2
export NXBOX_SHADER_CORPUS_EMITTER=/tmp/nxbox-corpus-build/nxbox-shader-corpus
python3 tools/nxbox/shader_corpus/run.py --report /tmp/nxbox-corpus-report.json
ctest --test-dir /tmp/nxbox-corpus-build --output-on-failure
```

On Windows use the `Release/nxbox-shader-corpus.exe` executable after building with
`--config Release`. `--corpus` accepts a directory or one cache/manifest file;
`NXBOX_SHADER_CORPUS` defaults to `~/.local/share/nxbox/corpus`. Inputs are read only.
`--dxc /path/to/dxc` or `DXC` enables DXIL validation with the appropriate SM 6.0
stage profile. DXC is optional; an explicitly requested missing compiler is an error.
`--timeout` limits each compiler invocation (default 60 seconds). `--output` retains
HLSL locally; otherwise all generated shader artifacts are temporary.

The table reports total compilation jobs, emitted programs, failed jobs, and DXC
passes/failures. A DXC failure counts as both emitted and failed. JSON includes every
job, stage, compiler diagnostics, exact failure reasons, and opcode counts when the
backend names its failing emitter. Unknown opcodes are never guessed from unrelated
IR instructions. Exit codes: **0** all jobs passed, **1** failures, **77** absent/empty
corpus. VertexA+VertexB pairs are translated separately then merged into one vertex
program and counted as one compilation job, matching the renderer.

## Supported capture formats

- OpenGL `opengl.bin`, guest cache version **15**.
- Vulkan `vulkan.bin`, guest cache version **18**.
- Current `NXBOX_DUMP_SHADERS` folders: `<pipeline-hash>/pipeline.txt` and
  `va|vb|tcs|tes|gs|fs.maxwell.bin` with `hash/address/start/bytes` manifest records.
- Older flat `<pipeline-hash>.txt` manifests with
  `<pipeline-hash>_s<index>_<shader-hash>.maxwell.bin` records.

Both guest caches use `yuzucach`, a little-endian u32 version, and repeated pipeline
records: u32 environment count, serialized environments, then the native pipeline
key. GL graphics keys occupy **624 bytes**, Vulkan graphics keys **1056 bytes**,
and compute keys **24 bytes**. These are `sizeof(Key)`, not the shorter hashing
`Key::Size()`; `SerializePipeline` writes the complete object. `--layout` checks the
host ABI against the Vulkan fixed-state and transform-feedback definitions.
Unknown versions fail explicitly. `yuzuvkch` driver caches are diagnosed as having
no guest environments; they cannot be recompiled into HLSL.

An environment contains five u64 lengths/counts, seven u32 state fields, guest code,
texture-type and texture-format maps, constant-buffer values and replacements, then
either the 80-byte SPH (plus geometry passthrough mask) or compute workgroup/shared
memory. The bounded readers preserve these fields and reject truncation, unaligned
code, overflow, excessive map sizes, duplicate keys, and invalid stages. A corrupt
tail does not discard preceding complete pipeline records or other files.

## Real behavior and limits

No shader translation or emission is stubbed. The private host logging/assertion
adapter is shared with `tools/hlsl-harness`; assertions throw in the worker. The
translation profile follows the D3D12 baseline with int64/fp64 and viewport-layer
support, SSBO alignment 16, and native fp16 lowering disabled. This is a compilation
coverage tool, not a pixel-accuracy oracle or exact pipeline-state replay: generic
inputs/color outputs are float, input topology/tessellation domain are triangles,
and runtime linkage/transform-feedback/alpha-test state is not reconstructed from
pipeline keys. Geometry/tessellation backend limitations remain reported failures.

**Current raw dumps do not capture texture maps, constant buffers, bindless slot,
viewport state, or geometry passthrough masks.** The adapter derives local memory
from SPH and uses the conventional proprietary-driver bindless slot 2 and viewport
transform enabled. It keeps uncaptured maps empty: a shader that needs them fails
with the exact missing lookup key instead of fabricated values. Geometry passthrough
and compute dumps with missing execution metadata fail explicitly. Use the full
on-disk guest caches for faithful environment replay. Even caches can omit values
that were never queried by their original backend; a new HLSL translation can expose
these missing entries.

## Tests and local data

```sh
cd tests/port
python3 -m unittest discover -s . -p 'test_*.py'
NXBOX_SHADER_CORPUS_EMITTER=/tmp/nxbox-corpus-build/nxbox-shader-corpus \
  python3 -m unittest test_shader_corpus -v
```

Parser, corruption, pairing, report accounting, and simulated compiler-failure tests
always run, including CI without a corpus. Tests generate a tiny synthetic Maxwell
EXIT corpus at runtime, with both cache layouts and raw dumps; no game data is used.
Setting `NXBOX_SHADER_CORPUS_EMITTER` also enables real frontend/HLSL integration,
native ABI checks, and optional DXC validation. CTest sets this automatically.
The optional user-corpus test skips if absent and checks accounting/continuation,
not that the evolving HLSL backend can already emit every captured program. Use the
CLI's nonzero exit status and JSON failure rows to gate backend coverage work.
Keep caches, shader dumps, emitted HLSL, keys, and reports outside the repository.
