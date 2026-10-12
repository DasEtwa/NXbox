"""Synthetic matrix evidence, lifecycle and ownership regressions."""

import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.nxbox.compat import matrix
from tools.nxbox.compat.run import ConsoleLock

FIXTURE = """[diag] GAME_RUNNING
GAME_FIRST_FRAME
GAME_PRESENT frames=150 seconds=5.0
GAME_PRESENT frames=120 seconds=5
GAME_PRESENT frames=90 seconds=5
THREAD_PROFILE tid=42 name=GuestCore0 cpu_percent=83.5 interval_ms=5000
THREAD_PROFILE host_wait=present total_us=180 calls=1 interval_ms=5000
D3D12 InfoQueue severity=WARNING id=5 synthetic warning
D3D12 InfoQueue severity=ERROR id=6 synthetic error
Renderer recovered exception what()=synthetic failure
"""


class EvidenceTests(unittest.TestCase):
    def test_native_evidence(self):
        result = matrix.parse_evidence(FIXTURE)
        self.assertEqual([s["fps"] for s in result["fps_per_5s"]], [30, 24, 18])
        self.assertEqual(len(result["thread_profile"]), 2)
        self.assertEqual(result["infoqueue_counts"], {"warning": 1, "error": 1})
        self.assertEqual(len(result["failure_lines"]), 1)
        self.assertFalse(result["fatal"])
        self.assertEqual(matrix.classify(result, [0.1, 0.2])[0], "in-game")

    def test_boot_and_malformed_fps(self):
        result = matrix.parse_evidence(
            "GAME_RUNNING\nGAME_PRESENT frames=abc seconds=5\nGAME_PRESENT frames=4 seconds=0"
        )
        self.assertEqual(matrix.classify(result, [1, 1])[0], "boot")
        self.assertFalse(result["infoqueue_available"])

    def test_static_and_animated_menu_uncertainty(self):
        result = matrix.parse_evidence(FIXTURE)
        self.assertEqual(matrix.classify(result, [0, 0])[0], "menus")
        self.assertIn("animated menus", matrix.classify(result, [0.1, 0.2])[1])

    def test_fatal_markers_and_crash_file(self):
        for line in (
            "GAME_FAIL failed",
            "terminate called",
            "Unhandled exception",
            "D3D12_REMOVED",
        ):
            with self.subTest(line=line):
                self.assertEqual(
                    matrix.classify(matrix.parse_evidence(FIXTURE + line), [0.2, 0.2])[0], "crashed"
                )
        self.assertEqual(
            matrix.classify(matrix.parse_evidence(FIXTURE, "synthetic crash"), [])[0], "crashed"
        )

    def test_runner_error_is_unknown_not_game_crash(self):
        result = matrix.parse_evidence(FIXTURE)
        result["runner_error"] = "network failure"
        self.assertEqual(matrix.classify(result, [0.2, 0.2])[0], "error")

    def test_sparse_or_slow_presentation_does_not_claim_gameplay(self):
        for text in ("GAME_PRESENT frames=150 seconds=5", "GAME_PRESENT frames=5 seconds=5\n" * 3):
            self.assertEqual(matrix.classify(matrix.parse_evidence(text), [1, 1])[0], "menus")

    def test_input_contains_periodic_a_start_and_stick(self):
        script = matrix.generic_input(180)
        self.assertGreater(script.count(":A:"), 15)
        self.assertGreater(script.count(":PLUS:"), 15)
        self.assertIn(":LSTICK_RIGHT:3", script)

    def test_image_changes_ignore_encoded_metadata(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow unavailable")

        def encoded(color):
            buf = io.BytesIO()
            Image.new("RGB", (8, 8), color).save(buf, format="PNG")
            return matrix.frame_signature(buf.getvalue())

        black, white = encoded("black"), encoded("white")
        self.assertEqual(matrix.frame_change(black, black), 0)
        self.assertEqual(matrix.frame_change(black, white), 1)


class InventoryPortal:
    def __init__(self):
        self.record = {
            "title_id": "0100000000000000",
            "name": "Synthetic",
            "path": "games\\synthetic.nsp",
        }

    def files(self, package, directory):
        return {"directory": directory}

    def json(self, endpoint, query):
        if query["directory"].endswith("library"):
            return {
                "Items": [{"Name": "0100000000000000.json"}, {"Name": "0100000000000000.mods.json"}]
            }
        return {
            "Items": [
                {"Name": name}
                for name in (
                    "synthetic.nsp",
                    "uncached.xci",
                    "compressed.nsz",
                    "../unsafe.nsp",
                    "keys.txt",
                )
            ]
        }

    def pull(self, *args):
        return json.dumps(self.record).encode()


class InventoryTests(unittest.TestCase):
    def test_all_local_packages_and_cache_deduplication(self):
        games, warnings = matrix.inventory(InventoryPortal(), "pkg")
        self.assertEqual(len(games), 3)
        self.assertFalse(warnings)
        self.assertEqual(len([g for g in games if g["source"] == "directory"]), 2)
        self.assertTrue(any(g["title_id"].startswith("unknown-") for g in games))

    def test_bad_cache_does_not_hide_local_game(self):
        portal = InventoryPortal()
        portal.record["path"] = "../unsafe.nsp"
        games, warnings = matrix.inventory(portal, "pkg")
        self.assertEqual(len(games), 3)
        self.assertEqual(len(warnings), 1)

    def test_external_cache_is_included(self):
        portal = InventoryPortal()
        portal.record["path"] = "E:\\NXbox\\games\\external.nsp"
        games, warnings = matrix.inventory(portal, "pkg")
        self.assertEqual(len(games), 4)
        self.assertEqual(len(warnings), 1)


class LifecycleTests(unittest.TestCase):
    def test_foreign_lock_is_never_removed_on_interrupt(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "lock"
            path.write_text("other-owner")
            with (
                patch("tools.nxbox.compat.run.time.sleep", side_effect=KeyboardInterrupt),
                self.assertRaises(KeyboardInterrupt),
                ConsoleLock(path),
            ):
                self.fail("Foreign lock acquired")
            self.assertEqual(path.read_text(), "other-owner")

    def test_replaced_lock_survives_release(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "lock"
            with ConsoleLock(path):
                path.unlink()
                path.write_text("new-owner")
            self.assertEqual(path.read_text(), "new-owner")

    def test_dry_run_avoids_credentials_lock_and_network_and_diffs(self):
        with tempfile.TemporaryDirectory() as temp:
            with (
                patch.object(matrix, "Portal", side_effect=AssertionError("network")),
                patch.object(matrix, "credentials", side_effect=AssertionError("credentials")),
                patch.object(matrix, "ConsoleLock", side_effect=AssertionError("lock")),
            ):
                args = ["--dry-run", "--sha", "ff72a7f02", "--artifacts", temp]
                self.assertEqual(matrix.main(args), 0)
                self.assertEqual(matrix.main(args), 0)
            runs = sorted(Path(temp).glob("matrix-*"))
            report = json.loads((runs[-1] / "compat.json").read_text())
            self.assertTrue(report["complete"])
            self.assertTrue(report["previous"])
            self.assertEqual(len(report["games"]), 2)
            self.assertFalse(any(c["regression"] for c in report["diff"]))
            self.assertEqual(len(list(runs[-1].rglob("contact-sheet.png"))), 3)
            self.assertTrue((runs[-1] / "diff.json").exists())

    def test_regression_in_state_fps_and_infoqueue(self):
        base = {
            "case_id": "id",
            "renderer": "opengl",
            "state": "in-game",
            "mean_fps": 30,
            "infoqueue_counts": {},
        }
        for update in ({"state": "menus"}, {"mean_fps": 20}, {"infoqueue_counts": {"error": 1}}):
            self.assertTrue(
                matrix.matrix_diff([dict(base, **update)], {"games": [base]})[0]["regression"]
            )
        self.assertEqual(matrix.matrix_diff([], {"games": [base]})[0]["change"], "missing")

    def test_install_waits_for_async_completion(self):
        from unittest.mock import MagicMock

        portal = MagicMock()
        portal.request.side_effect = [b"", b'{"Code":3805991009}', b'{"Code":0}', b""]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic.appx"
            path.write_bytes(b"synthetic package")
            with (
                patch.object(matrix.time, "sleep"),
                patch.object(matrix.time, "monotonic", side_effect=[0, 0, 0, 3, 6, 10, 10]),
            ):
                matrix.install(portal, [path])
        self.assertEqual(portal.request.call_count, 4)
        self.assertEqual(portal.request.call_args_list[0].args[2], {"package": "synthetic.appx"})

    def test_install_failure_is_not_success(self):
        from unittest.mock import MagicMock

        portal = MagicMock()
        portal.request.side_effect = [b"", b'{"Code":42}']
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic.appx"
            path.write_bytes(b"synthetic")
            with (
                patch.object(matrix.time, "sleep"),
                self.assertRaisesRegex(RuntimeError, "code=42"),
            ):
                matrix.install(portal, [path])


class LiveMockTests(unittest.TestCase):
    def test_manifest_identity_selects_supplied_version(self):
        import zipfile
        from unittest.mock import MagicMock

        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic.appx"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(
                    "AppxManifest.xml",
                    '<Package xmlns="urn:synthetic"><Identity Name="NSPX.NXbox" Version="1.2.3.4" ProcessorArchitecture="x64" /></Package>',
                )
            prefix = matrix.package_identity(path)
        portal = MagicMock()
        portal.json.return_value = {
            "InstalledPackages": [
                {"PackageFullName": "NSPX.NXbox_9.9.9.9_x64__publisher"},
                {"PackageFullName": prefix + "_publisher"},
            ]
        }
        self.assertEqual(
            matrix.installed_package(portal, prefix)["PackageFullName"], prefix + "_publisher"
        )
        with self.assertRaises(RuntimeError):
            matrix.installed_package(portal, "NSPX.NXbox_0.0.0.0_x64_")

    def run_lifecycle(self, interrupted=False):
        from test_compat_runner import FakePortal

        class Portal(FakePortal):
            def json(self, endpoint, query=None):
                if endpoint == "/api/app/packagemanager/packages":
                    return {
                        "InstalledPackages": [
                            {"PackageFullName": "NSPX.NXbox_1.2.3.4_x64__publisher"}
                        ]
                    }
                if query and query.get("path") == "LocalState/settings":
                    return {"Items": [{"Name": "0100000000000000.txt"}]}
                return super().json(endpoint, query)

            def request(self, *args):
                return b""

        portal = Portal()
        original = {
            ("LocalState", "eden_settings.txt"): b"backend=5\nresolution_setup=2\n",
            ("LocalState/settings", "0100000000000000.txt"): b"renderer_backend=5\n",
            ("LocalState", "game.url"): b"https://invalid.example/synthetic",
            ("LocalState", "game.txt"): b"original.nsp",
            ("LocalState", "nxbox_env.txt"): b"NXBOX_PRESENT_INTERVAL=2\n",
        }
        portal.storage.update(original)
        games = [
            {
                "title_id": "0100000000000000",
                "name": "Synthetic",
                "file_name": "synthetic.nsp",
                "source": "library",
            }
        ]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            appx = root / "synthetic.appx"
            appx.write_bytes(b"synthetic")
            lock = root / "lock"
            calls = []

            def installed(*args):
                self.assertTrue(lock.exists())

            def case(portal, package, game, output, renderer, seconds):
                self.assertTrue(lock.exists())
                self.assertNotIn(("LocalState", "game.url"), portal.storage)
                self.assertEqual(
                    portal.storage["LocalState/settings", "0100000000000000.txt"].strip(),
                    b"renderer_backend=4",
                )
                self.assertIn(
                    b"renderer_backend=4", portal.storage["LocalState", "eden_settings.txt"]
                )
                calls.append(renderer)
                if interrupted and len(calls) == 2:
                    raise KeyboardInterrupt
                return matrix.dry_case(game, output, renderer)

            with (
                patch.object(matrix, "Portal", return_value=portal),
                patch.object(matrix, "credentials", return_value=("host", "user", "password")),
                patch.object(matrix, "ConsoleLock", return_value=ConsoleLock(lock)),
                patch.object(matrix, "install", side_effect=installed),
                patch.object(matrix, "package_identity", return_value="NSPX.NXbox_1.2.3.4_x64_"),
                patch.object(matrix, "inventory", return_value=(games, [])),
                patch.object(matrix, "run_case", side_effect=case),
                patch.object(matrix, "require_pillow"),
            ):
                args = [
                    "--sha",
                    "ff72a7f02",
                    "--appx",
                    str(appx),
                    "--artifacts",
                    str(root / "artifacts"),
                ]
                if interrupted:
                    with self.assertRaises(KeyboardInterrupt):
                        matrix.main(args)
                else:
                    self.assertEqual(matrix.main(args), 0)
            self.assertFalse(lock.exists())
            for key, value in original.items():
                self.assertEqual(portal.storage[key], value)
            self.assertNotIn(("LocalState", "skip_library.txt"), portal.storage)
            report_path = next((root / "artifacts").glob("matrix-*/compat.json"))
            report = json.loads(report_path.read_text())
            self.assertEqual(report["complete"], not interrupted)
            self.assertTrue((report_path.parent / "previous-controls/manifest.json").exists())
            self.assertEqual(calls, ["opengl", "d3d12"])

    def test_live_mock_restores_controls_and_lock(self):
        self.run_lifecycle()

    def test_interrupt_restores_controls_and_lock_and_partial_report(self):
        self.run_lifecycle(interrupted=True)

    def test_snapshot_mode_disables_guest_dump_and_preserves_evidence(self):
        from test_compat_runner import FakePortal

        from tools.nxbox.compat.run import run_game

        portal = FakePortal()
        with tempfile.TemporaryDirectory() as temp:
            captures = []
            result = run_game(
                portal,
                {"PackageFullName": "NXbox"},
                {"title_id": "id", "file_name": "synthetic.nsp"},
                Path(temp),
                0.001,
                0.001,
                "NXBOX_THREAD_PROFILE=1",
                "5:A:0.3",
                1,
                snapshot=lambda elapsed, folder: captures.append(elapsed),
            )
            self.assertTrue(captures)
            self.assertFalse(result["errors"])
            self.assertIn(b"NXBOX_FRAME_DUMP=0", portal.storage["LocalState", "nxbox_env.txt"])
            self.assertEqual(portal.stops, 2)


if __name__ == "__main__":
    unittest.main()
