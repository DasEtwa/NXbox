# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""The DXIL compiler must lower shuffle_xor/up/down, which nir_to_dxil cannot emit."""

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))
FILE = "src/microsoft/compiler/nir_to_dxil.c"


@unittest.skipUnless(SOURCE.exists(), "pinned Mesa checkout not available")
class ShuffleLoweringTests(unittest.TestCase):
    def test_lowering_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            dst = Path(tmp) / FILE
            dst.parent.mkdir(parents=True)
            shutil.copy(SOURCE / FILE, dst)
            sys.path.insert(0, str(ROOT / "tools/nxbox"))
            import patch_mesa_uwp

            import inspect

            self.assertIn("patch_dxil_shuffle(root)", inspect.getsource(patch_mesa_uwp.patch))
            patch_mesa_uwp.patch_dxil_shuffle(Path(tmp))
            text = dst.read_text()
            self.assertIn("nir_lower_subgroups, &nxbox_subgroup_options", text)
            self.assertIn(".lower_relative_shuffle = true", text)
            self.assertIn(".lower_shuffle_to_32bit = true", text)
            self.assertIn(".lower_to_scalar = true", text)
            self.assertLess(
                text.index("NXBOX shuffle lowering"),
                text.index("nir_opt_load_store_vectorize, &vectorize_opts"),
            )


if __name__ == "__main__":
    unittest.main()
