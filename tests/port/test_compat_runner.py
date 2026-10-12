"""Host regressions for compatibility evidence, scoring and console ownership."""

import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.nxbox.compat.diag import parse_diag, score_game
from tools.nxbox.compat.images import contact_sheet, png, read_ppm
from tools.nxbox.compat.portal import Portal, PortalError
from tools.nxbox.compat.run import ConsoleLock, load_games, main, reports, run_game


class DiagnosticsTests(unittest.TestCase):
    def test_native_lines_and_interval_counts(self):
        result = parse_diag("""GAME_RUNNING
GAME_FIRST_FRAME
MEM boot commit=512 MiB limit=5120 MiB
GAME_PRESENT frames=150 seconds=5.100 fps=29.41 memory=805306368 memory_limit=5368709120 tick_ms=12345
GAME_PRESENT frames=0 seconds=5.000 memory=700000000
GAME_FAIL device removed
GAME_LOAD_FAILED status=3
""")
        self.assertTrue(result["boot_ok"])
        self.assertTrue(result["first_frame"])
        self.assertEqual(result["memory_peak_bytes"], 805306368)
        self.assertEqual([p["frames"] for p in result["presents"]], [150, 0])
        self.assertAlmostEqual(result["presents"][0]["presents_per_5s"], 150 * 5 / 5.1)
        self.assertEqual(len(result["game_fail"]), 2)
        self.assertAlmostEqual(result["presents"][1]["window_start_seconds"], 5.1)
        self.assertAlmostEqual(result["presents"][1]["window_end_seconds"], 10.1)

    def test_missing_and_malformed_evidence(self):
        result = parse_diag("""NOT_GAME_RUNNING
GAME_PRESENT frames=-1 seconds=5
GAME_PRESENT frames=10 seconds=0
GAME_PRESENT frames=abc seconds=5
GAME_FIRST_FRAME_EXTRA
GAME_PRESENT frames=10
""")
        self.assertFalse(result["boot_ok"])
        self.assertFalse(result["first_frame"])
        self.assertEqual(result["presents"], [])
        self.assertIsNone(result["memory_peak_bytes"])
        self.assertEqual(score_game(result), 0)

    def test_scores_stalls_failures_and_crashes(self):
        result = parse_diag("GAME_RUNNING\nGAME_FIRST_FRAME\nGAME_PRESENT frames=150 seconds=5")
        result["frames"] = ["frame.ppm"]
        self.assertEqual(score_game(result), 100)
        result["presents"].append({"frames": 0, "seconds": 5})
        self.assertEqual(score_game(result), 75)
        result["crash_file"] = "nxbox_crash.txt"
        self.assertEqual(score_game(result), 20)
        result["crash_file"] = None
        result["game_fail"] = ["GAME_FAIL failure"]
        self.assertEqual(score_game(result), 20)
        result["runner_error"] = "network failure"
        self.assertEqual(score_game(result), 0)

    def test_boot_without_frames_is_not_full_compatibility(self):
        self.assertEqual(score_game(parse_diag("GAME_RUNNING")), 20)
        self.assertEqual(score_game(parse_diag("GAME_PRESENT frames=0 seconds=5")), 0)


class ImageTests(unittest.TestCase):
    def test_ppm_comments_and_whitespace_raster(self):
        pixels = b"\n #\xff\0\x80"
        self.assertEqual(read_ppm(b"P6\n# comment\n2 1\n255\n" + pixels), (2, 1, pixels))
        self.assertEqual(read_ppm(b"P6\r\n2 1\r\n255\r\n" + pixels), (2, 1, pixels))
        with self.assertRaises(ValueError):
            read_ppm(b"P6\n2 1\n255\nabc")

    def test_png_crc_and_raster_roundtrip(self):
        pixels = b"\xff\0\x80\0\x10\x20"
        encoded = png(2, 1, pixels)
        self.assertEqual(encoded[:8], b"\x89PNG\r\n\x1a\n")
        offset = 8
        while offset < len(encoded):
            size = struct.unpack(">I", encoded[offset : offset + 4])[0]
            kind = encoded[offset + 4 : offset + 8]
            data = encoded[offset + 8 : offset + 8 + size]
            crc = struct.unpack(">I", encoded[offset + 8 + size : offset + 12 + size])[0]
            self.assertEqual(crc, zlib.crc32(kind + data) & 0xFFFFFFFF)
            if kind == b"IDAT":
                self.assertEqual(zlib.decompress(data), b"\0" + pixels)
            offset += size + 12

    def test_sheet_contains_pixels(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            frame = root / "frame.ppm"
            frame.write_bytes(b"P6\n1 1\n255\n\xff\0\0")
            sheet = root / "sheet.png"
            contact_sheet([frame], sheet, columns=1, tile_width=1, tile_height=1)
            self.assertEqual(sheet.read_bytes(), png(1, 1, b"\xff\0\0"))


class LockTests(unittest.TestCase):
    def test_waits_for_even_empty_foreign_lock(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "console.lock"
            path.write_bytes(b"")
            with (
                patch("tools.nxbox.compat.run.time.sleep", side_effect=KeyboardInterrupt),
                self.assertRaises(KeyboardInterrupt),
                ConsoleLock(path),
            ):
                self.fail("Acquired another owner's lock")
            self.assertTrue(path.exists())

    def test_release_only_owned_token(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "console.lock"
            with ConsoleLock(path):
                self.assertIn(b"nxbox-compat", path.read_bytes())
            self.assertFalse(path.exists())
            with ConsoleLock(path):
                path.write_text("another-owner\n")
            self.assertEqual(path.read_text(), "another-owner\n")


class FakePortal:
    def __init__(self):
        self.storage = {
            ("LocalState", "eden_uwp_diag.txt"): b"GAME_FAIL previous game\n",
            ("LocalState", "nxbox_crash.txt"): b"old crash",
        }
        self.stops = 0
        self.fail_launch = False

    def stop(self, package):
        self.stops += 1

    def files(self, package, directory):
        return {"path": directory}

    def json(self, endpoint, query=None):
        if endpoint == "/api/app/packagemanager/packages":
            return {"InstalledPackages": [{"PackageFullName": "NXbox", "Name": "NXbox"}]}
        return {
            "Items": [
                {"Name": name} for (directory, name) in self.storage if directory == query["path"]
            ]
        }

    def pull(self, package, name, directory="LocalState", optional=False):
        return self.storage.get((directory, name))

    def delete(self, package, name, directory="LocalState"):
        self.storage.pop((directory, name), None)

    def push(self, package, name, data, directory="LocalState"):
        self.storage[directory, name] = data

    def launch(self, package):
        if self.fail_launch:
            raise PortalError("POST", "/api/taskmanager/app", 500)
        self.storage["LocalState", "eden_uwp_diag.txt"] = (
            b"GAME_RUNNING\nGAME_FIRST_FRAME\nGAME_PRESENT frames=150 seconds=5 memory=1000\n"
        )
        self.storage["LocalState/framedump", "frame_000150.ppm"] = b"P6\n1 1\n255\n\xff\0\0"


class RunnerTests(unittest.TestCase):
    def run_fake(self, portal, root):
        return run_game(
            portal,
            {"PackageFullName": "NXbox"},
            {"title_id": "0100000000000000", "file_name": "game.nsp"},
            root,
            0.001,
            0.001,
            "",
            "10:A:1",
            40,
        )

    def test_probe_isolates_evidence_and_emits_reports(self):
        portal = FakePortal()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            result = self.run_fake(portal, root)
            self.assertEqual(result["score"], 100)
            self.assertEqual(result["game_fail"], [])
            self.assertIsNone(result["crash_file"])
            self.assertIsNotNone(result["first_frame_seconds"])
            self.assertEqual(portal.stops, 2)
            self.assertEqual(portal.storage["LocalState", "game.txt"], b"games\\game.nsp\n")
            self.assertIn(
                b"NXBOX_INPUT_SCRIPT=10:A:1", portal.storage["LocalState", "nxbox_env.txt"]
            )
            reports(root, [result], "NXbox", 5)
            self.assertEqual(
                json.loads((root / "compat.json").read_text())["games"][0]["score"], 100
            )
            self.assertTrue((root / result["title_id"] / "contact-sheet.png").is_file())
            self.assertEqual(
                (root / result["title_id"] / "previous/nxbox_crash.txt").read_bytes(), b"old crash"
            )

    def test_launch_failure_still_stops_and_scores_zero(self):
        portal = FakePortal()
        portal.fail_launch = True
        with tempfile.TemporaryDirectory() as temp:
            result = self.run_fake(portal, Path(temp))
            self.assertEqual(result["score"], 0)
            self.assertEqual(portal.stops, 2)
            self.assertIn("500", result["runner_error"])

    def test_main_restores_previous_controls_and_releases_lock(self):
        portal = FakePortal()
        portal.storage["LocalState", "nxbox_env.txt"] = b"NXBOX_PRESENT_INTERVAL=2\n"
        portal.storage["LocalState", "game.txt"] = b"original.nsp\n"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            games = root / "games.json"
            games.write_text(
                json.dumps([{"title_id": "0100000000000000", "file_name": "game.nsp"}])
            )
            lock = root / "console.lock"
            with (
                patch(
                    "tools.nxbox.compat.run.credentials", return_value=("host", "user", "password")
                ),
                patch("tools.nxbox.compat.run.Portal", return_value=portal),
                patch("tools.nxbox.compat.run.ConsoleLock", return_value=ConsoleLock(lock)),
            ):
                self.assertEqual(
                    main(
                        [
                            "--games",
                            str(games),
                            "--output",
                            str(root / "run"),
                            "--seconds",
                            "0.001",
                            "--poll",
                            "0.001",
                        ]
                    ),
                    0,
                )
            self.assertFalse(lock.exists())
            self.assertEqual(portal.storage["LocalState", "game.txt"], b"original.nsp\n")
            self.assertEqual(
                portal.storage["LocalState", "nxbox_env.txt"], b"NXBOX_PRESENT_INTERVAL=2\n"
            )
            self.assertNotIn(("LocalState", "skip_library.txt"), portal.storage)
            self.assertTrue((root / "run/compat.md").is_file())

    def test_preflight_failure_does_not_collect_stale_evidence(self):
        portal = FakePortal()
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(portal, "delete", side_effect=PortalError("DELETE", "/file", 503)):
                result = self.run_fake(portal, Path(temp))
            self.assertEqual(result["game_fail"], [])
            self.assertEqual(result["score"], 0)
            self.assertIsNone(result["crash_file"])
            self.assertEqual(result["frames"], [])
            self.assertEqual(portal.stops, 2)

    def test_stop_failure_retains_results(self):
        portal = FakePortal()
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(
                portal, "stop", side_effect=[None, PortalError("DELETE", "/process", 503)]
            ):
                result = self.run_fake(portal, Path(temp))
            self.assertTrue(result["stop_failed"])
            self.assertEqual(result["score"], 0)
            self.assertTrue(result["frames"])

    def test_game_list_validates_ids_and_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "games.json"
            path.write_text(json.dumps([{"title_id": "01000000000000ab", "file_name": "game.nsp"}]))
            self.assertEqual(load_games(path)[0]["title_id"], "01000000000000AB")
            path.write_text(
                json.dumps([{"title_id": "01000000000000ab", "file_name": "../game.nsp"}])
            )
            with self.assertRaises(ValueError):
                load_games(path)


class PortalTests(unittest.TestCase):
    def test_rooted_paths_and_query_contract(self):
        portal = Portal("127.0.0.1", "user", "secret")
        self.assertEqual(portal.base, "https://127.0.0.1:11443")
        self.assertEqual(
            portal.files("pkg", "LocalState\\framedump", "frame.ppm"),
            {
                "knownfolderid": "LocalAppData",
                "packagefullname": "pkg",
                "path": "/LocalState/framedump",
                "filename": "frame.ppm",
            },
        )
        with patch.object(portal, "request", return_value=b"") as request:
            portal.push("pkg", "skip_library.txt", b"")
            args = request.call_args.args
            self.assertEqual(args[0], "POST")
            self.assertIn(b'filename="skip_library.txt"', args[3])
            portal.launch({"PackageFullName": "pkg", "PackageRelativeId": "App"})
            self.assertEqual(request.call_args.args[2], {"appid": "QXBw", "package": "cGtn"})

    def test_mutations_fetch_csrf_and_keep_auth_in_headers(self):
        portal = Portal("127.0.0.1", "user", "secret")
        portal.cookies = [SimpleNamespace(name="CSRF-Token", value="csrf-value")]
        response = MagicMock()
        response.__enter__.return_value.read.return_value = b"{}"
        with patch.object(portal.opener, "open", return_value=response) as opened:
            portal.request("POST", "/api/taskmanager/app", {"package": "encoded"})
            portal.request("DELETE", "/api/taskmanager/app", {"package": "encoded"})
        self.assertEqual(opened.call_count, 3)
        requests = [call.args[0] for call in opened.call_args_list]
        self.assertEqual(requests[0].get_method(), "GET")
        self.assertTrue(requests[0].full_url.endswith("/api/os/machinename"))
        self.assertEqual(requests[1].get_header("X-csrf-token"), "csrf-value")
        self.assertEqual(requests[2].get_header("X-csrf-token"), "csrf-value")
        self.assertNotIn("secret", requests[1].full_url)
        self.assertTrue(requests[1].get_header("Authorization").startswith("Basic "))

    def test_optional_missing_does_not_hide_authentication_failure(self):
        portal = Portal("127.0.0.1", "user", "secret")
        with patch.object(portal, "request", side_effect=PortalError("GET", "/file", 404)):
            self.assertIsNone(portal.pull("pkg", "missing", optional=True))
        with (
            patch.object(portal, "request", side_effect=PortalError("GET", "/file", 401)),
            self.assertRaises(PortalError),
        ):
            portal.pull("pkg", "missing", optional=True)


if __name__ == "__main__":
    unittest.main()
