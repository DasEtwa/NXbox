# SPDX-License-Identifier: GPL-3.0-or-later
"""Upload witnesses must distinguish CPU bytes, GPU source bytes and cache copies."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_copy_write_hazards import function_body

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class UploadTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (NATIVE / "d3d12_buffer_cache.cpp").read_text()
        cls.copy = function_body(cls.source, "void BufferCacheRuntime::CopyBuffer(")

    def test_opt_in_and_download_witnesses_do_not_recurse(self):
        toggle = function_body(self.source, "bool UploadTraceEnabled()")
        self.assertIn('BisectEnabled("NXBOX_D3D12_UPLOAD_TRACE")', toggle)
        self.assertIn("static const bool enabled", toggle)
        self.assertIn(
            "const bool trace = UploadTraceEnabled() && dst->heap == D3D12_HEAP_TYPE_DEFAULT;",
            self.copy,
        )
        allocation = function_body(self.copy, "if (trace)")
        self.assertIn("D3D12_HEAP_TYPE_READBACK", allocation)
        self.assertIn("CheckedAdd(trace_size, copy.size)", allocation)
        self.assertIn("CheckedMultiply(trace_size, 2)", allocation)
        self.assertLess(self.copy.index("InvalidRange(1"), self.copy.index("const bool trace"))

    def test_full_cpu_snapshot_precedes_allocation_and_actual_recording(self):
        snapshot = self.copy.index("src->mapped.data() + copy.src_offset")
        allocation = self.copy.index("context.CreateBuffer(CheckedMultiply")
        recording = self.copy.index("context.Transition(src,")
        self.assertLess(snapshot, allocation)
        self.assertLess(allocation, recording)
        self.assertIn("trace_copies.assign(copies.begin(), copies.end());", self.copy)
        self.assertIn("trace_expected.resize(trace_size);", self.copy)
        self.assertIn("cursor += copy.size;", self.copy)

    def test_source_and_destination_probes_bracket_the_original_copy(self):
        before = self.copy.index("CopyBuffer(trace_readback, src, trace_reads, true);")
        original = self.copy.index("context.Commands()->CopyBufferRegion(")
        after = self.copy.index("CopyBuffer(trace_readback, dst, trace_writes, true);")
        self.assertLess(before, original)
        self.assertLess(original, after)
        self.assertLess(after, self.copy.index("context.AfterCompletion("))
        self.assertIn("trace_reads.push_back({copy.src_offset, cursor, copy.size});", self.copy)
        self.assertIn(
            "trace_writes.push_back({copy.dst_offset, trace_size + cursor, copy.size});",
            self.copy,
        )
        self.assertIn("context.Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);", self.copy[after:])

    def test_completion_owns_bytes_and_never_records_or_waits(self):
        callback = self.copy[self.copy.index("context.AfterCompletion(") :]
        for capture in (
            "trace_readback, src, trace_id, trace_tick, trace_size",
            "copies = std::move(trace_copies)",
            "expected = std::move(trace_expected)",
        ):
            self.assertIn(capture, callback)
        for field in (
            "D3D12_UPLOAD_RESULT",
            "cpu_hash=",
            "gpu_source_hash=",
            "gpu_destination_hash=",
            "source_diff=",
            "destination_diff=",
            "staging_diff=",
            "window=",
        ):
            self.assertIn(field, callback)
        self.assertIn("trace_readback->mapped.subspan(cursor, copy.size)", callback)
        self.assertIn("trace_size + cursor, copy.size", callback)
        for forbidden in ("this", "context.", "Wait(", "Finish(", "Commands(", "gpu_memory"):
            self.assertNotIn(forbidden, callback.removeprefix("context.AfterCompletion("))

    def test_resource_history_includes_creation_clear_join_and_binding(self):
        self.assertIn("D3D12_UPLOAD_CREATE resource={:#x} guest_base={:#x}", self.source)
        self.assertIn("D3D12_UPLOAD_CLEAR resource={:#x}", self.source)
        self.assertIn("D3D12_UPLOAD_BIND slot={} resource={:#x}", self.source)
        self.assertIn("D3D12_UPLOAD_COPY id={} tick={} source={:#x}", self.copy)
        # DEFAULT-to-DEFAULT joins get GPU witnesses even without a CPU mapping.
        allocation = function_body(self.copy, "if (trace)")
        self.assertLess(
            allocation.index("if (src->heap == D3D12_HEAP_TYPE_UPLOAD)"),
            allocation.index("if (trace_size)"),
        )
        join = function_body(
            (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text(),
            "void BufferCache<P>::JoinOverlap(",
        )
        self.assertIn("runtime.CopyBuffer(new_buffer, overlap,", join)

    def test_measured_offsets_are_visible_even_for_matching_copies(self):
        callback = self.copy[self.copy.index("context.AfterCompletion(") :]
        self.assertIn("for (const u64 probe : {u64{72}, u64{26968}})", callback)
        self.assertIn("probe < copy.dst_offset || probe - copy.dst_offset >= copy.size", callback)
        self.assertIn("const size_t relative = probe - copy.dst_offset;", callback)
        self.assertIn("D3D12_UPLOAD_PROBE id={} destination={:#x} offset={}", callback)
        for source in ("cpu", "gpu_source", "gpu_destination"):
            self.assertIn(f"TraceHex({source}, relative)", callback)

    def test_byte_comparison_detects_zero_upload_at_measured_offsets(self):
        helpers = "\n".join(
            signature + " {" + function_body(self.source, signature) + "}"
            for signature in (
                "u64 TraceHash(std::span<const u8> bytes)",
                "size_t TraceDifference(std::span<const u8> expected, std::span<const u8> actual)",
            )
        )
        driver = """
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
using u8 = std::uint8_t;
using u64 = std::uint64_t;
__HELPERS__
int main() {
    std::vector<u8> expected(65536), source(65536), destination(65536);
    for (const size_t offset : {size_t{72}, size_t{26968}}) {
        expected[offset] = 0x80;
        source = expected;
        assert(TraceDifference(expected, source) == expected.size());
        assert(TraceDifference(source, destination) == offset);
        assert(TraceHash(expected) == TraceHash(source));
        assert(TraceHash(source) != TraceHash(destination));
        source = destination;
        assert(TraceDifference(expected, source) == offset);
        assert(TraceDifference(source, destination) == source.size());
        expected[offset] = 0;
    }
}
""".replace("__HELPERS__", helpers)
        with tempfile.TemporaryDirectory(prefix="nxbox-upload-trace-") as folder:
            source = Path(folder) / "trace.cpp"
            binary = Path(folder) / "trace"
            source.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
