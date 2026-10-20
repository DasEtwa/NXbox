# Mario Party device-removal probe

The supplied `compat-2026-10-18/r-marioparty` artifacts run **Mario Party
Superstars**, title `01006FE013472000`. They establish a reproducible device
removal, but do not identify the offending D3D12 call.

- `eden.txt`: at 35.66 s, an 8x8 single-layer 2D view cannot satisfy a cube
  declaration and is replaced with a null SRV. At 35.70–35.88 s, frozen fragment
  shader translation encounters guest handle `0xffffffff` outside its descriptor
  pool. At 36.21 s, fence completion detects removal with `0x887a0001`.
- `diag.txt`: presentation reaches approximately 59.8 fps; the renderer catches
  the failure and stops the game. DRED returns no breadcrumb nodes or allocation
  records and reports address zero. That output does not distinguish an actual
  null-address access from an absent fault record.
- `crash.txt`: records the same renderer exception. Memory remains below the
  application limit; this is not evidence of a memory-limit process termination.
- `driver.log`: input continues after the failure. Input timestamps do not locate
  an offending graphics command.

The pool warning comes from shader translation, not a native heap lookup. Native
texture bindings use planned, sequential slots. Empty root CBVs already receive
an allocated zero buffer; query reduction's unused metadata SRV already receives
a valid results-buffer address. Query result buffers are sized for all banks;
query report copies validate the source range and use 4 or 8 bytes. Transitions
retain their resources through fence completion, and recorded barriers also hold
COM references until replay. Texture blits check regions and copy compatibility;
reinterpretation uses a buffer with separate source/destination footprints.
These observations narrow candidates but cannot prove what the failing run sent.

The previous SRV resource-dimension and null-sampler-anisotropy fixes are
unchanged. No new root-cause claim is made from their temporal proximity.

## Next console run

Use a package built from this worktree with:

```text
NXBOX_D3D12_VALIDATE_COMMANDS=1
NXBOX_D3D12_SUBMIT_TRACE=1
```

The compatibility runner accepts these as repeated `--env-d3d12 KEY=VALUE`
arguments. Preserve the existing title/input sequence. Submit trace serializes
submissions, so this is a diagnostic run, not a performance measurement.

Expected diagnostics:

- `D3D12_VALIDATION before_execute tick=...`: counters immediately before each
  submission, including deferred replay. Match the tick to the submit-trace label.
- `D3D12_VALIDATION invalid tick=... call=...`: zero/misaligned root CBV addresses,
  zero root SRV/UAV addresses, tables outside currently bound heaps, buffer-copy
  bounds, query resolve bounds/alignment, or malformed texture-copy regions.
  The probe logs arguments and throws before publishing that invalid API call.
  Batched graphics root bindings use the same checks.
- `unwritten_slot=...`: a draw descriptor plan failed to write a slot in this
  draw, even if its scratch storage retained a previous descriptor key.
- `null_srv`: the newly created null SRV's dimension and description fields.
- `texture_copy`: formats, extents, offsets and subresources, including copies
  whose format compatibility needs further inspection. BC edge padding and
  format plane counts are accounted for in region validation.
- `zero_size_copy`: an empty copy is logged without rejection; empty texture
  boxes can legally be no-ops.

If a check fires, its call and arguments provide the missing evidence for a
specific correction. If removal still occurs with no invalid-argument diagnostic,
the final serialized tick identifies the list to investigate; this probe does not
validate every D3D12 rule (for example, shader access bounds, root-signature
compatibility, all format-family rules, or GPU resource lifetime after replay).
Do not interpret a clean probe or the defensive table-slice bounds check as a
confirmed fix for the 36-second failure.
