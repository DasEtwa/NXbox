# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Protected async PSOs get one bounded wait before a draw is skipped.
Skipped draws are lost for good when the game renders them once: SM3DW bakes
its level ambient light at load and the level stayed black on Xbox (async on)."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp"
POOL = ROOT / "src/video_core/renderer_d3d12/d3d12_async_pso.h"


class PsoWait(unittest.TestCase):
    def test_both_skip_paths_wait_once_first(self):
        source = CACHE.read_text()
        for counter in ("DrawsSkippedPendingPso", "DispatchesSkippedPendingPso"):
            skip = source.rindex(f"Count(FrameCount::{counter})")
            before = source[skip - 2200 : skip]
            self.assertIn("pso_waited.insert(", before)
            self.assertIn("async->WaitFor(", before)

    def test_unadmitted_requests_keep_their_wait(self):
        # E2 (Wonder): 28 of 50 waits came back Missing because the pool was full; the
        # draw spent its only wait without waiting and was skipped for good afterwards.
        source = CACHE.read_text()
        for counter in ("DrawsSkippedPendingPso", "DispatchesSkippedPendingPso"):
            skip = source.rindex(f"Count(FrameCount::{counter})")
            before = source[skip - 2600 : skip]
            wait = before.index("async->WaitFor(")
            self.assertIn("if (settled.status == AsyncCache::Status::Missing)", before[wait:])
            self.assertIn("pso_waited.erase(", before[wait:])

    def test_workers_wake_waiters(self):
        pool = POOL.read_text()
        self.assertIn("Entry WaitFor(", pool)
        self.assertIn("state.finished.notify_all();", pool)
        self.assertIn("finished.wait_for(", pool)


if __name__ == "__main__":
    unittest.main()
