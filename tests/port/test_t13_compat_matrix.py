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
            matrix.classify(matrix.parse_evidence(FIXTURE, "event=exception code=0xc0000005"), [])[
                0
            ],
            "crashed",
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

    def run_lifecycle(self, interrupted=False, crashcontrol_failure=False):
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
                if crashcontrol_failure:
                    from tools.nxbox.compat.portal import PortalError

                    raise PortalError("DELETE", "/api/debug/dump/usermode/crashcontrol", 500)
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

            def case(portal, package, game, output, renderer, seconds, environment=None):
                self.assertIn("NXBOX_TEST=two", environment)
                self.assertEqual("NXBOX_ONLY=1" in environment, renderer == "d3d12")
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
                    "--env",
                    "NXBOX_TEST=one",
                    "--env",
                    "NXBOX_TEST=two",
                    "--env-d3d12",
                    "NXBOX_ONLY=1",
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
            if crashcontrol_failure:
                self.assertTrue(any("500" in warning for warning in report["warnings"]))

    def test_crashcontrol_500_continues_and_restores(self):
        self.run_lifecycle(crashcontrol_failure=True)

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


class ConsoleRegressionTests(unittest.TestCase):
    def test_routine_crash_log_is_not_fatal(self):
        text = "event=memory commit=512\nCreateEvent GpuChannelSMExceptionBreakpoint42\nevent=exception code=0\nevent=exception code=0x80000003\n"
        result = matrix.parse_evidence(FIXTURE, text)
        self.assertFalse(result["fatal"])
        self.assertEqual(matrix.classify(result, [0.2, 0.2])[0], "in-game")
        for line in (
            "event=terminate code=1",
            "event=exception code=0xc0000005",
            "renderer_exception what=synthetic",
        ):
            self.assertTrue(matrix.parse_evidence(FIXTURE, line)["fatal"])

    def test_groups_updates_dlc_unknown_and_base_type(self):
        def package(title, kind=None):
            result = {"title_id": title, "file_name": title + ".nsp", "name": title}
            if kind:
                result["content_type"] = kind
            return result

        packages = [
            package("0100000000001000", "base"),
            package("0100000000001800", "update"),
            package("0100000000002001", "dlc"),
            package("unknown-123"),
            package("0100000000004000", "dlc"),
        ]
        games, groups = matrix.group_titles(packages)
        self.assertEqual([g["title_id"] for g in games], ["0100000000001000"])
        self.assertEqual(len(games[0]["additional_content"]), 2)
        self.assertEqual(sum(len(g["additional_content"]) for g in groups), 4)

    def test_renderer_env_defaults_and_overrides(self):
        common = [matrix.env_assignment("NXBOX_TEST=a=b"), "NXBOX_D3D12_DEBUG=0"]
        gl = matrix.case_environment("opengl", common, ["NXBOX_ONLY=1"])
        dx = matrix.case_environment("d3d12", common, ["NXBOX_ONLY=1"])
        self.assertNotIn("NXBOX_ONLY", gl)
        self.assertIn("NXBOX_ONLY=1", dx)
        self.assertIn("NXBOX_D3D12_DEBUG=0", dx)
        self.assertIn("NXBOX_D3D12_FRAME_PROFILE=1", dx)
        self.assertIn("NXBOX_D3D12_DEBUG=1", matrix.case_environment("d3d12"))
        import argparse

        for bad in ("NO_EQUALS", "1KEY=value", "KEY=a\nB=b", "KEY=a\0"):
            with self.assertRaises(argparse.ArgumentTypeError):
                matrix.env_assignment(bad)

    def test_previous_run_skips_empty_wrong_type_and_invalid(self):
        import os

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reports = [
                {"schema_version": 2, "dry_run": False, "games": [{"case_id": "id"}]},
                {"schema_version": 2, "dry_run": False, "games": [], "complete": False},
                {"schema_version": 2, "dry_run": True, "games": [{}]},
            ]
            for i, report in enumerate(reports):
                folder = root / f"matrix-{i}"
                folder.mkdir()
                path = folder / "compat.json"
                path.write_text(json.dumps(report))
                os.utime(path, (i + 1, i + 1))
            folder = root / "matrix-broken"
            folder.mkdir()
            (folder / "compat.json").write_text("{")
            path, _ = matrix.previous_run(root, False)
            self.assertEqual(path.parent.name, "matrix-0")

    def test_frame_profile_draws_are_used(self):
        result = matrix.parse_evidence(
            FIXTURE + "D3D12_FRAME_PROFILE presents=150 draw_calls=300\n"
        )
        self.assertEqual(result["draws_per_frame"], [2])
        self.assertEqual(matrix.classify(result, [0.2, 0.2])[0], "menus")

    def test_draws_and_black_capture_classification(self):
        for draws, expected in ((2, "menus"), (20, "in-game")):
            evidence = matrix.parse_evidence(
                (f"GAME_PRESENT frames=150 seconds=5 draws={draws * 150}\n") * 3
            )
            self.assertEqual(evidence["draws_per_frame"], [draws] * 3)
            self.assertEqual(matrix.classify(evidence, [0.2, 0.2])[0], expected)
        evidence["black_frames"] = [False, True, True]
        self.assertEqual(matrix.classify(evidence, [0.2, 0.2])[0], "black-screen")
        evidence["black_frames"] = [True, False, True]
        self.assertEqual(matrix.classify(evidence, [0.2, 0.2])[0], "in-game")
        self.assertTrue(matrix.near_black(bytes([3]) * 1728))
        self.assertFalse(matrix.near_black(bytes([20]) * 1728))
        self.assertFalse(matrix.near_black(b""))

    def test_process_exit_and_new_dump_evidence(self):
        from test_compat_runner import FakePortal

        from tools.nxbox.compat.run import run_game

        for disappeared in (False, True):
            portal = FakePortal()
            portal.running = lambda _, exited=disappeared: not exited
            with (
                tempfile.TemporaryDirectory() as temp,
                patch.object(
                    portal,
                    "dump_names",
                    create=True,
                    side_effect=[{"old.dmp"}, {"old.dmp", "new.dmp"}],
                ),
                patch("tools.nxbox.compat.run.time.monotonic", side_effect=range(100)),
                patch("tools.nxbox.compat.run.time.sleep"),
            ):
                result = run_game(
                    portal,
                    {"PackageFullName": "NXbox"},
                    {"title_id": "id", "file_name": "synthetic.nsp"},
                    Path(temp),
                    10,
                    1,
                    "",
                    "",
                    1,
                    monitor=True,
                )
                self.assertEqual(result["new_dumps"], ["new.dmp"])
                self.assertEqual(bool(result.get("process_disappeared")), disappeared)
                result.update(matrix.parse_evidence(FIXTURE))
                self.assertEqual(matrix.classify(result, [1, 1])[0], "crashed")
                self.assertEqual(portal.stops, 2)

    def test_run_case_uses_screenshots_and_preserves_nonfatal_logs(self):
        from unittest.mock import MagicMock

        from tools.nxbox.compat.images import png

        game = {
            "case_id": "synthetic",
            "title_id": "0100000000000000",
            "file_name": "fixture.nsp",
            "name": "Synthetic",
        }
        portal = MagicMock()
        portal.request.side_effect = [
            png(1, 1, color) for color in (b"\xff\xff\xff", b"\x03\x03\x03", b"\x03\x03\x03")
        ]

        def run(*args, **kwargs):
            folder = args[3] / args[2]["title_id"]
            folder.mkdir()
            self.assertIn("NXBOX_D3D12_DEBUG=1", args[6])
            self.assertTrue(kwargs["monitor"])
            for elapsed in (0, 30, 60):
                kwargs["snapshot"](elapsed, folder)
            (folder / "eden_uwp_diag.txt").write_text(FIXTURE)
            (folder / "nxbox_crash.txt").write_text(
                "CreateEvent GpuChannelSMExceptionBreakpoint42\nevent=memory commit=10\n"
            )
            return {"errors": []}

        with (
            tempfile.TemporaryDirectory() as temp,
            patch.object(matrix, "run_game", side_effect=run),
        ):
            result = matrix.run_case(
                portal, {"PackageFullName": "NXbox"}, game, Path(temp), "d3d12", 90
            )
            self.assertEqual(result["state"], "black-screen")
            self.assertEqual(result["black_frames"], [False, True, True])
            self.assertEqual(len(result["frame_changes"]), 2)
            self.assertFalse(result["fatal"])
            self.assertEqual(len(result["frames"]), 3)

    def test_dump_inventory_is_package_scoped(self):
        from tools.nxbox.compat.portal import Portal

        portal = Portal("127.0.0.1", "user", "synthetic")
        payload = {
            "CrashDumps": [
                {"PackageFullName": "NXbox", "Dumps": [{"FileName": "old.dmp"}]},
                {"PackageFullName": "Other", "FileName": "foreign.dmp"},
            ]
        }
        with patch.object(portal, "json", return_value=payload):
            self.assertEqual(portal.dump_names("NXbox"), {"old.dmp"})
        with patch.object(
            portal, "json", return_value={"Processes": [{"PackageFullName": "NXbox"}]}
        ):
            self.assertTrue(portal.running("NXbox"))
            self.assertFalse(portal.running("Other"))


if __name__ == "__main__":
    unittest.main()
