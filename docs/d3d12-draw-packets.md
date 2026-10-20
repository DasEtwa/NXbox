# Compact draw recording

The record stream now writes value-only draw packets directly, without the generic
argument pack, array alignment padding, producer allocation, or `std::function`.
The indexed and non-indexed packets share a 24-byte representation. A singleton
occupies 44 bytes (16-byte header, 4-byte count, 24-byte draw), versus 48 bytes for
the previous generic encoding on 64-bit hosts. A run of N draws occupies
20 + 24*N bytes instead of 48*N and uses one consumer dispatch/scratch resize.
Each native draw is still issued separately and in the original order.

Appending is legal only in the producer's unpublished chunk. Every ordinary
write and every Flush ends the run, including state changes, resource barriers,
queries, indirect execution, and submission controls. Crossing a chunk boundary
also ends the run. No command or header already visible to the recorder changes.
Split singleton packets continue through the existing SPSC backpressure path.
`NXBOX_D3D12_COMPACT_DRAWS=0` restores the previous draw encoder. This optimization
is enabled by default and does not change graphics state lifetime.

## Experimental transport state reuse

`NXBOX_D3D12_RECORD_STATE=1` additionally suppresses equal viewport, scissor, blend,
stencil, topology, vertex-view, and index-view setter commands in the facade.
This is distinct from the rasterizer's existing guest state comparison. In
particular, it can reuse dynamic state across conservative guest invalidations
without retaining root bindings, PSOs, render targets, or descriptors. All helper
passes using the facade update the same transport state. Command-list creation
resets it on the producer, and ExecuteIndirect discards it because an indirect
signature may change and clear IA views. It is disabled by default because the
console previously needed conservative invalidation to fix rendering.

Float representations are compared byte-for-byte, preserving signed zero and NaN
payloads. Standard stencil references update both faces. Vertex views cache only
the most recently written range; changing its start invalidates that comparison.
Null pointers invalidate comparisons and are forwarded. No descriptors or
buffer/texture caches are modified.

## Evidence and expected benefit

The host test in `tests/port/test_d3d12_draw_packets.py` compares the production
facade against the old EncodeCall path at every observable command, using 72,000
random indexed/non-indexed draws with repeated and changing dynamic state,
partial vertex ranges, random float bit patterns, indirect commands, publication,
and list resets. It checks ordered effective state and draw arguments, not just
command counts. It also checks zero producer allocations, tiny chunk recycling,
abandoned execution, and runs under ASan/UBSan and TSan. Existing recorder tests
cover COM ownership, descriptor snapshots, failure drain, and submit handoff.

There is no new per-draw allocation or type-erased callback to remove from the
old implementation: it was already allocation-free on the producer. Savings
come from fewer serialization writes, fewer payload bytes, and fewer consumer
command dispatches/native setters.

At 3,580 draws/frame, the supplied 7.5 ms recording phase is about 2.10 us/draw.
A planning estimate is **0.01–0.05 us/draw** for compact packets alone and
**0.2–0.5 us/draw** with transport reuse when most of the seven dynamic setters
repeat (about 0.7–1.8 ms/frame). The latter assumes roughly 30–70 ns avoided per
setter across encoding and replay, net of comparison overhead. These are estimates,
not console measurements: actual setter repetition and batching opportunities
must be profiled on Series X. If queries/barriers intervene between every draw,
there is no batching benefit; only the singleton packet reduction remains.
