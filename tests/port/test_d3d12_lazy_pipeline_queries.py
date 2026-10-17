# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Pipeline statistics on every draw filled the 256-slot query bank ~14 times per frame in BotW,
and each full bank made the CPU wait for the GPU. Only the guest's ZPass counter is ever enabled,
so statistics are recorded only after the guest reads a pipeline counter, and draws that record
nothing take no bank slot. BisectEnabled is cached because it ran several times per draw."""

import re
import unittest
from pathlib import Path

D3D12 = Path(__file__).resolve().parents[2] / "src/video_core/renderer_d3d12"


class LazyPipelineQueries(unittest.TestCase):
    def test_begin_draw_skips_unused_queries(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        begin = re.search(r"void QueryCacheRuntime::BeginDraw\(bool geometry\) \{(.*?)\n\}", source, re.S).group(1)
        self.assertIn("impl->pipeline_wanted", begin)
        self.assertLess(begin.index("if (!draw.recorded)"), begin.index("BeginQuery"))
        end = re.search(r"void QueryCacheRuntime::EndDraw\(\) \{(.*?)\n\}", source, re.S).group(1)
        self.assertLess(end.index("if (!draw.recorded)"), end.index("++impl->count"))
        read = re.search(
            r"QueryCacheRuntime::CounterResult QueryCacheRuntime::ReadCounter\(.*?\{(.*?)\n\}",
            source,
            re.S,
        ).group(1)
        self.assertIn("impl->pipeline_wanted = true;", read)

    def test_full_bank_rotates_without_finish(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        prepare = re.search(r"void QueryCacheRuntime::PrepareDraw\(\) \{(.*?)\n\}", source, re.S).group(1)
        self.assertIn("impl->Rotate();", prepare)
        self.assertNotIn("Collect", prepare)
        rotate = re.search(r"void Rotate\(\) \{(.*?)\n    \}", source, re.S).group(1)
        self.assertNotIn("Finish", rotate)

    def test_bisect_enabled_is_cached(self):
        source = (D3D12 / "d3d12_context.cpp").read_text()
        body = re.search(r"bool BisectEnabled\(const char\* name\) noexcept \{(.*?)\n\}", source, re.S).group(1)
        self.assertIn("thread_local", body)


if __name__ == "__main__":
    unittest.main()
