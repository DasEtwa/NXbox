# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the UWP OpenSSL install cache tied to its package and toolchain."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE = (ROOT / "CMakeLists.txt").read_text()


class OpenSSLCacheTests(unittest.TestCase):
    def test_workflow_key_and_install_path(self):
        workflow = (ROOT / ".github/workflows/build-nxbox.yml").read_text()
        cache = workflow.split("- name: Cache built OpenSSL for UWP", 1)[1].split("- uses:", 1)[0]
        cache = " ".join(cache.split())
        self.assertIn("uses: actions/cache@v4", cache)
        self.assertIn("path: .cache/openssl-uwp", cache)
        self.assertNotIn("restore-keys:", cache)
        for field in ("msvc", "sdk", "version", "hash"):
            self.assertIn("${{ steps.openssl-key.outputs." + field + " }}", cache)
        self.assertIn("x64-MD-Release", cache)
        self.assertIn("json.load(open('cpmfile.json'))['openssl']", workflow)
        self.assertIn("p['version']", workflow)
        self.assertIn("p['hash']", workflow)
        self.assertIn('vcvarsall.bat" x64 uwp', workflow)
        self.assertIn("%VCToolsVersion%", workflow)
        self.assertIn("NXBOX_OPENSSL_CACHE: ${{ github.workspace }}\\.cache\\openssl-uwp", workflow)
        self.assertIn('cmake --install build-uwp --prefix "$env:NXBOX_OPENSSL_CACHE"', workflow)
        self.assertIn("--component nxbox-openssl", workflow)
        self.assertIn('"${OpenSSL_SOURCE_DIR}/include/" "${OpenSSL_BINARY_DIR}/include/"', CMAKE)
        self.assertIn('install(FILES "${OPENSSL_CRYPTO_LIBRARY}" "${OPENSSL_SSL_LIBRARY}"', CMAKE)
        self.assertIn("if (OpenSSL_ADDED)\n        AddJsonPackage(openssl-cmake)", CMAKE)
        self.assertIn("if (OpenSSL_ADDED OR NXBOX_OPENSSL_CACHE_HIT)", CMAKE)

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required")
    def test_cmake_uses_complete_uwp_prefix_only(self):
        hook = CMAKE.split("# openssl funniness\n", 1)[1].split("if (YUZU_USE_BUNDLED_OPENSSL)", 1)[
            0
        ]
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary) / "install"
            files = ["include/openssl/ssl.h", "lib/libcrypto.lib", "lib/libssl.lib"]
            for relative in files:
                path = prefix / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            script = Path(temporary) / "check.cmake"
            script.write_text(
                hook
                + "\nif (NXBOX_OPENSSL_CACHE_HIT)\n"
                + "  if (NOT OpenSSL_FORCE_SYSTEM OR NOT OPENSSL_USE_STATIC_LIBS "
                + "OR OPENSSL_MSVC_STATIC_RT OR NOT OPENSSL_ROOT_DIR STREQUAL expected_prefix)\n"
                + '    message(FATAL_ERROR "Wrong cached OpenSSL configuration")\n'
                + "  endif()\n"
                + "endif()\n"
                + "if (NOT NXBOX_OPENSSL_CACHE_HIT STREQUAL expected_hit)\n"
                + '  message(FATAL_ERROR "Wrong cache branch")\n'
                + "endif()\n"
            )
            for system, complete, expected in [
                ("WindowsStore", True, "ON"),
                ("Windows", True, "OFF"),
                ("WindowsStore", False, "OFF"),
            ]:
                with self.subTest(system=system, complete=complete):
                    if not complete:
                        (prefix / files[-1]).unlink()
                    subprocess.run(
                        [
                            "cmake",
                            f"-DCMAKE_SYSTEM_NAME={system}",
                            f"-Dexpected_prefix={prefix.as_posix()}",
                            f"-Dexpected_hit={expected}",
                            "-P",
                            str(script),
                        ],
                        env={**os.environ, "NXBOX_OPENSSL_CACHE": str(prefix)},
                        capture_output=True,
                        text=True,
                        check=True,
                    )


if __name__ == "__main__":
    unittest.main()
