# SPDX-License-Identifier: GPL-3.0-or-later
"""The memstream and DXIL-unsupported patches apply to the pinned Mesa copy."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_mesa_full_chain import ROOT, SOURCE


@unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
class MesaMemstreamTests(unittest.TestCase):
    def test_patches(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-memstream-test-") as temp:
            mesa = Path(temp) / "mesa"
            shutil.copytree(SOURCE, mesa, ignore=shutil.ignore_patterns(".git"))
            subprocess.run(
                [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(mesa)],
                check=True, capture_output=True, text=True, timeout=60,
            )
            mem = (mesa / "src/util/memstream.c").read_text()
            self.assertIn("wchar_t path[MAX_PATH]", mem)
            self.assertIn("GetTempPathW(MAX_PATH, path)", mem)
            self.assertIn("GetTempFileNameW(path, L\"MEMSTREAM\", 0, wtemp)", mem)
            self.assertNotIn("GetTempPath(", mem)
            self.assertNotIn("GetTempFileName(", mem)
            dxil = (mesa / "src/microsoft/compiler/nir_to_dxil.c").read_text()
            self.assertIn('SetEnvironmentVariableA("NXBOX_DXIL_UNSUPPORTED", msg)', dxil)
            self.assertLess(dxil.index("nxbox_publish_unsupported(msg);"),
                            dxil.index("logger->log(logger->priv, msg);"))

    def test_frontend_collects_variable(self):
        text = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn('"NXBOX_DXIL_UNSUPPORTED"', text)


if __name__ == "__main__":
    unittest.main()
