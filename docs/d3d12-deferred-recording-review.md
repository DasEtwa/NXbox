# Deferred D3D12 recording review

Reviewed the default deferred path on `review/deferred-recording`, including the
command facade, stream/argument encoding, descriptor snapshots, Context submission
and retirement, descriptor/staging rings, query readbacks, fence manager, Present,
resize and cache teardown. This is a host review; the supplied console A/B result
is not a new measurement from this work.

## Confirmed defects fixed

1. **Recorder startup could terminate the process.** `StartRecorder` named the
   thread and set its priority outside its exception boundary. The MSVC thread-name
   implementation calls `UTF8ToUTF16W`, which can allocate and throw. Such an
   exception escaped the `std::thread` entry point instead of reaching `WaitReady`.
   Both setup calls now run inside the existing initialization try/catch. A harness
   extracts the actual production `StartRecorder` method, injects exceptions at
   naming, priority, list initialization and interface acquisition, and verifies
   failure propagation and a joined worker. Before the fix, both sanitizer variants
   terminated with `uncaught exception ... startup failure`.
2. **Scratch allocation failure lost stream framing.** `Consume` removed the header
   before resizing consumer scratch storage. If resize threw, the worker's failure
   drain treated that record's payload as the next header, potentially allocating
   an arbitrary size or invoking an invalid replay pointer. The consumer now keeps
   a pending header until the payload is copied successfully, and clears it before
   replay so a throwing command is still released exactly once. Fault injection
   checks two abandoned records and their COM holds, plus 10,000 records through a
   three-chunk, 31-byte pool with concurrent failure draining and shutdown. Before
   the fix, the sanitizer runs failed (including a truncated-record exception).

## Ordering and ownership audit

- Chunk `used` release/acquire transfers immutable bytes to the consumer and
  transfers writable storage back to the producer. The atomic availability counter
  prevents missed wakeups; Close publishes the partial chunk before EOF. Scratch
  and the new pending header are consumer-only. Existing stress coverage includes
  30,000 records, payloads larger than a chunk, partial flushes and backpressure.
- `EncodeCall` acquires all argument holds before writing a header that might be
  published at a chunk boundary. Arrays, marker data and copy/barrier structures
  are copied. Active barrier unions and embedded copy-location resources have
  explicit leases. Replay releases holds on success, abandonment and exceptions.
- RTV/DSV snapshot slots are copied on the producer and released after their list
  method returns. Their busy flag synchronizes reuse; exhaustion flushes before
  waiting. Shader-visible descriptor slots remain leased to the logical tick until
  `IsFree` sees both acknowledgement and GPU completion. Retaining a heap alone is
  not used as permission to reuse its slots.
- Wrapper/placed-resource leases move into completion batches before publication
  of the submit command. `CompleteThrough` clamps to acknowledged ticks, and its
  callers supply GPU-completed ticks. Staging pages require both fence retirement
  and sole wrapper ownership before reuse. PSO/root ownership remains with cache
  drains or completion captures, beyond the stream's shorter COM hold.
- Only the recorder mutates native allocator slots in deferred mode. It waits for
  the previous slot's fence, resets, replays, closes, executes, signals and then
  acknowledges. Producer tick advancement is separate. The producer uses the
  allocator array's fixed size, not its concurrently changing index or ticks.
- `Wait` seals open producer work; `WaitSubmitted` only awaits acknowledgement and
  GPU completion. Descriptor/staging pressure and query/readback callers use these
  boundaries. The recorder's allocator wait does not invoke completion callbacks.
  Profile timestamp reuse additionally waits for callback retirement.
- Present awaits acknowledgement before DXGI Present; per-buffer descriptor reuse,
  resize and normal teardown wait for GPU completion. Context closes and joins the
  recorder before member destruction. Initialization failure closes and joins it
  before rethrowing. Replay failure switches to consuming without executing so
  producer chunk/descriptor waits can finish and encoded holds can be released.

No additional data race or premature descriptor/ring reuse was reproduced in the
portable components. This does not establish that native D3D12 lifetimes are safe
under every failure condition.

## Remaining risks and verification limits

- Persistent memory exhaustion can still prevent scratch growth even during the
  failure drain. Preserving the header fixes corruption after allocation failure;
  it does not guarantee shutdown progress if allocation never succeeds. No native
  sustained-OOM test was performed.
- A producer-side submission failure makes `Acknowledge` throw even if earlier
  acknowledged lists remain in flight. Teardown's Finish can therefore fail before
  those earlier lists have completed. Premature native GPU object destruction on
  that path is suspected; the fake-list harness cannot prove driver behavior, and
  this review does not change the existing device-loss/failed-teardown policy.
- Full Context and its callers were not run under TSan: the portable components and
  extracted startup method were. Native completion callbacks, DRED/InfoQueue,
  fence events/timeouts and swapchain interactions still need Windows/Xbox fault
  injection, including title switching, screenshots, resize and device removal.
- MSVC/UWP compilation, the listed warning-as-error set, console rendering and
  performance were not verified on this Mac. No push or console deployment was
  performed.

## Validation

The existing stress harness in
`tests/port/test_d3d12_deferred_recording.py` already runs separate ASan/UBSan and
ThreadSanitizer binaries; it was extended rather than duplicated. Both new defects
were reproduced before changing production code.

```text
python3 -B -m unittest discover -s tests/port -p 'test_d3d12_deferred_recording.py'
Ran 4 tests in 17.037s
OK
```

Both sanitizer variants reported successful facade argument/lifetime checks,
12,288 snapshot reuses, 30,000 SPSC records, zero producer encoding allocations,
failed-replay draining, scratch-allocation failure recovery/backpressure/shutdown,
96 submissions with acknowledgement distinct from GPU completion, and startup
failure propagation/join. Neither emitted sanitizer diagnostics.

Formatting used the repository's clang-format configuration for changed C++ lines
and `ruff format` for the Python harness. `ruff format --check` passed. `ruff check`
reports the same six pre-existing diagnostics as HEAD (I001, three B023 and two
PLW1510); no unrelated lint cleanup was included.

```text
python3 -B -m unittest discover -s tests/port -p 'test_d3d12*.py'
Ran 231 tests in 121.895s
OK (skipped=2)

git diff --check
(no output; exit 0)
```

The two skipped census tests require `NXBOX_PSO_CENSUS_DIR` and the read-only c24
artifact folder. The initial suite run passed 229 tests with the same two skips.

The requested commit could not be created: `git add` failed with `Operation not
permitted` while creating the worktree's `index.lock`. Its Git metadata is outside
this session's writable directory. The four changed files remain in the current
worktree, unstaged. Intended commit message: `Fix deferred D3D12 recorder exception
handling`; requested author/committer: `Miguel <miguel@nspx.dev>`. Nothing was pushed.
