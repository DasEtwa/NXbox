"""Run with python3 -m tools.nxbox.compat.run --games games.json --output results."""

import argparse
import json
import os
import re
import signal
import sys
import time
import uuid
from pathlib import Path

from .diag import fatal_lines, parse_diag, score_game
from .images import contact_sheet, read_ppm
from .portal import Portal, PortalError, credentials


class ConsoleLock:
    def __init__(self, path=Path("/tmp/xbox-console.lock"), interval=5):
        self.path = Path(path)
        self.interval = interval
        self.token = f"nxbox-compat {os.getpid()} {uuid.uuid4().hex}\n".encode()
        self.identity = None

    def __enter__(self):
        announced = False
        while True:
            try:
                descriptor = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            except FileExistsError:
                if not announced:
                    print("Waiting for /tmp/xbox-console.lock", flush=True)
                    announced = True
                time.sleep(self.interval)
                continue
            with os.fdopen(descriptor, "wb") as handle:
                self.identity = os.fstat(handle.fileno()).st_ino
                handle.write(self.token)
            return self

    def __exit__(self, *args):
        try:
            if self.path.stat().st_ino == self.identity and self.path.read_bytes() == self.token:
                self.path.unlink()
        except FileNotFoundError:
            pass


def load_games(path):
    games = json.loads(path.read_text())
    if not isinstance(games, list) or not games:
        raise ValueError("Games must be a nonempty JSON array")
    titles = set()
    for game in games:
        if not isinstance(game, dict) or not re.fullmatch(
            r"[0-9a-fA-F]{16}", game.get("title_id", "")
        ):
            raise ValueError("Each game needs a 16-digit hexadecimal title_id")
        title = game["title_id"].upper()
        if title in titles:
            raise ValueError("Duplicate title_id")
        titles.add(title)
        game["title_id"] = title
        filename = game.get("file_name")
        if not isinstance(filename, str) or not filename or any(c in filename for c in "\r\n\0"):
            raise ValueError("Each game needs a single-line file_name")
        if any(part == ".." for part in filename.replace("\\", "/").split("/")):
            raise ValueError("Game paths must not contain parent traversal")
        script = game.get("input_script", "")
        if not isinstance(script, str) or any(c in script for c in "\r\n\0"):
            raise ValueError("input_script must be a single line")
    return games


def select_package(portal, name):
    packages = portal.json("/api/app/packagemanager/packages").get("InstalledPackages", [])
    matches = [
        p
        for p in packages
        if p["PackageFullName"] == name
        or p.get("Name") == name
        or (name == "NXbox" and p["PackageFullName"].startswith("NSPX.NXbox_"))
    ]
    if not matches:
        raise RuntimeError("NXbox package was not found")

    def version(package):
        match = re.search(r"_(\d+)\.(\d+)\.(\d+)\.(\d+)_", package["PackageFullName"])
        return tuple(map(int, match.groups())) if match else (0, 0, 0, 0)

    return max(matches, key=version)


def frame_names(portal, package):
    try:
        items = portal.json(
            "/api/filesystem/apps/files", portal.files(package, "LocalState/framedump")
        )
    except PortalError as error:
        if error.status == 404:
            return []
        raise
    return sorted(
        {
            item["Name"]
            for item in items.get("Items", [])
            if re.fullmatch(r"frame_\d+(?:_phase_\d+)?\.ppm", item.get("Name", ""))
        }
    )


def safe_error(error):
    # Do not echo remote response bodies, credential records or URLs.
    if isinstance(error, PortalError):
        return str(error)
    return f"Runner operation failed ({type(error).__name__})"


def run_game(
    portal,
    package,
    game,
    output,
    seconds,
    poll,
    environment,
    script,
    max_frames,
    snapshot=None,
    monitor=False,
):
    full_name = package["PackageFullName"]
    folder = output / game["title_id"]
    folder.mkdir()
    previous = folder / "previous"
    previous.mkdir()
    result = {
        "title_id": game["title_id"],
        "file_name": game["file_name"],
        "name": game.get("name", game["file_name"]),
        "duration_seconds": seconds,
        "first_frame_seconds": None,
        "first_frame_poll_seconds": poll,
        "frames": [],
        "crash_file": None,
        "errors": [],
    }
    evidence_ready = False
    old_dumps = None
    result["warnings"] = []
    if monitor:
        try:
            old_dumps = portal.dump_names(full_name)
        except PortalError as error:
            result["warnings"].append("Dump inventory unavailable: " + safe_error(error))
    try:
        portal.stop(full_name)
        # Archive stale evidence before clearing it so it cannot score the next game.
        for name, directory in (
            ("eden_uwp_diag.txt", "LocalState"),
            ("nxbox_crash.txt", "LocalState"),
            ("eden_log.txt", "LocalState/eden/log"),
        ):
            old = portal.pull(full_name, name, directory, optional=True)
            if old is not None:
                (previous / name).write_bytes(old)
                portal.delete(full_name, name, directory)
        for name in frame_names(portal, full_name):
            (previous / name).write_bytes(portal.pull(full_name, name, "LocalState/framedump"))
            portal.delete(full_name, name, "LocalState/framedump")
        evidence_ready = True
        filename = game["file_name"]
        target = (
            filename
            if ":" in filename or "/" in filename or "\\" in filename
            else "games\\" + filename
        )
        env = environment.rstrip() + (
            "\nNXBOX_FRAME_DUMP=0\n" if snapshot else "\nNXBOX_FRAME_DUMP=1\n"
        )
        selected_script = game.get("input_script", script)
        if selected_script:
            env += f"NXBOX_INPUT_SCRIPT={selected_script}\n"
        for name, data in (
            ("nxbox_env.txt", env.encode()),
            ("game.txt", (target + "\n").encode()),
            ("skip_library.txt", b""),
        ):
            portal.push(full_name, name, data)
        start = time.monotonic()
        portal.launch(package)
        next_snapshot = 0.0
        observed_process = False
        while True:
            elapsed = time.monotonic() - start
            if monitor and elapsed < seconds:
                running = portal.running(full_name)
                if not running and (observed_process or elapsed >= min(15, seconds / 2)):
                    result["process_disappeared"] = True
                    break
                observed_process = observed_process or running
            if snapshot and elapsed >= next_snapshot:
                try:
                    snapshot(elapsed, folder)
                except Exception as error:  # noqa: BLE001 - Keep collecting after capture failures.
                    result["errors"].append(safe_error(error))
                next_snapshot = elapsed + 30
            data = portal.pull(full_name, "eden_uwp_diag.txt", optional=True)
            if data is not None:
                (folder / "eden_uwp_diag.txt").write_bytes(data)
                if (
                    result["first_frame_seconds"] is None
                    and parse_diag(data.decode("utf-8", "replace"))["first_frame"]
                ):
                    result["first_frame_seconds"] = time.monotonic() - start
            remaining = seconds - (time.monotonic() - start)
            if remaining <= 0:
                break
            time.sleep(min(poll, remaining))
    except Exception as error:  # noqa: BLE001 - Preserve evidence and redact errors at boundaries.
        result["runner_error"] = safe_error(error)
    finally:
        if monitor and old_dumps is not None and evidence_ready:
            try:
                result["new_dumps"] = sorted(portal.dump_names(full_name) - old_dumps)
            except PortalError as error:
                result["warnings"].append("Dump inventory unavailable: " + safe_error(error))
        # Attempt every evidence pull independently, then stop even when collection fails.
        try:
            if evidence_ready:
                for name, directory in (
                    ("eden_uwp_diag.txt", "LocalState"),
                    ("nxbox_crash.txt", "LocalState"),
                    ("eden_log.txt", "LocalState/eden/log"),
                ):
                    try:
                        data = portal.pull(full_name, name, directory, optional=True)
                        if data is not None:
                            (folder / name).write_bytes(data)
                            if name == "nxbox_crash.txt" and fatal_lines(
                                data.decode("utf-8", "replace")
                            ):
                                result["crash_file"] = f"{game['title_id']}/{name}"
                    except Exception as error:  # noqa: BLE001 - Preserve evidence and redact errors at boundaries.
                        result["errors"].append(safe_error(error))
                try:
                    names = frame_names(portal, full_name)
                    result["available_frame_count"] = len(names)
                    for name in names[:max_frames]:
                        data = portal.pull(full_name, name, "LocalState/framedump")
                        (folder / name).write_bytes(data)
                        try:
                            read_ppm(data)
                            result["frames"].append(f"{game['title_id']}/{name}")
                        except ValueError:
                            result["errors"].append(f"Invalid PPM: {name}")
                except Exception as error:  # noqa: BLE001 - Preserve evidence and redact errors at boundaries.
                    result["errors"].append(safe_error(error))
        finally:
            try:
                portal.stop(full_name)
            except Exception as error:  # noqa: BLE001 - Preserve evidence and redact errors at boundaries.
                result["runner_error"] = safe_error(error)
                result["stop_failed"] = True
    path = folder / "eden_uwp_diag.txt"
    result.update(parse_diag(path.read_text(errors="replace") if path.exists() else ""))
    result["score"] = score_game(result)
    return result


def reports(output, results, package, seconds):
    document = {
        "schema_version": 1,
        "package": package,
        "duration_seconds": seconds,
        "score_description": "Evidence score (0-100); visual correctness requires review",
        "games": results,
    }
    (output / "compat.json").write_text(json.dumps(document, indent=2) + "\n")
    escape = lambda value: str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")
    lines = [
        "# NXbox compatibility report",
        "",
        f"Package: `{package}`",
        "",
        "Scores measure boot/presentation evidence, not visual correctness or playability.",
        "First-frame times are launch-to-observation upper bounds (including portal latency).",
        "Memory peaks are sampled app usage, not continuous measurements.",
        "",
        "| Game | Title ID | Boot | First frame (s) | Peak MiB | Score |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for game in results:
        first = game["first_frame_seconds"]
        peak = game["memory_peak_bytes"]
        lines.append(
            f"| {escape(game['name'])} | {game['title_id']} | {game['boot_ok']} | "
            f"{f'{first:.2f}' if first is not None else 'unknown'} | "
            f"{f'{peak / (1 << 20):.1f}' if peak is not None else 'unknown'} | {game['score']} |"
        )
    for game in results:
        lines.extend(
            [
                "",
                f"## {escape(game['name'])}",
                "",
                "Presents/5 s: "
                + (", ".join(f"{s['presents_per_5s']:.1f}" for s in game["presents"]) or "none"),
                "",
                f"![Frames]({game['title_id']}/contact-sheet.png)",
                "",
                "Tiles in reading order: "
                + (", ".join(Path(p).name for p in game["frames"]) or "none (empty sheet)"),
            ]
        )
        lines.extend(
            [
                "",
                "| Window (s) | Presents | Presents/5 s | Tick (ms) |",
                "| --- | --- | --- | --- |",
            ]
        )
        for sample in game["presents"]:
            lines.append(
                f"| {sample['window_start_seconds']:.2f}–{sample['window_end_seconds']:.2f} | "
                f"{sample['frames']} | {sample['presents_per_5s']:.1f} | {sample.get('tick_ms', 'unknown')} |"
            )
        if game.get("crash_file"):
            lines.extend(["", f"Crash evidence: [{game['crash_file']}]({game['crash_file']})"])
        failures = game["game_fail"] + game["errors"]
        if game.get("runner_error"):
            failures.append(game["runner_error"])
        if failures:
            lines.extend(["", "Failures:", ""] + [f"- {escape(line)}" for line in failures])
        contact_sheet(
            [output / p for p in game["frames"]], output / game["title_id"] / "contact-sheet.png"
        )
    (output / "compat.md").write_text("\n".join(lines) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--games", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New directory for run evidence")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--poll", type=float, default=1)
    parser.add_argument("--max-frames", type=int, default=40)
    parser.add_argument("--package", default="NXbox")
    parser.add_argument("--env-file", type=Path)
    parser.add_argument("--input-script", default=os.environ.get("NXBOX_INPUT_SCRIPT", ""))
    args = parser.parse_args(argv)
    if not (0 < args.seconds <= 86400 and 0 < args.poll <= 60 and 0 < args.max_frames <= 1000):
        parser.error("seconds must be 0..86400, poll 0..60, max-frames 1..1000")
    if any(c in args.input_script for c in "\r\n\0"):
        parser.error("input-script must be a single line")
    games = load_games(args.games)
    environment = args.env_file.read_text() if args.env_file else ""
    portal = Portal(*credentials())
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    with ConsoleLock():
        package = select_package(portal, args.package)
        full_name = package["PackageFullName"]
        saved = {
            name: portal.pull(full_name, name, optional=True)
            for name in ("nxbox_env.txt", "game.txt", "skip_library.txt")
        }
        controls = args.output / "previous-controls"
        controls.mkdir()
        for name, data in saved.items():
            if data is not None:
                (controls / name).write_bytes(data)
        (controls / "manifest.json").write_text(
            json.dumps({name: data is not None for name, data in saved.items()}, indent=2) + "\n"
        )
        try:
            for game in games:
                print(f"Running {game['title_id']} for {args.seconds:g} seconds", flush=True)
                result = run_game(
                    portal,
                    package,
                    game,
                    args.output,
                    args.seconds,
                    args.poll,
                    environment,
                    args.input_script,
                    args.max_frames,
                )
                results.append(result)
                reports(args.output, results, full_name, args.seconds)
                if result.get("stop_failed"):
                    break
        finally:
            try:
                portal.stop(full_name)
            finally:
                errors = []
                for name, data in saved.items():
                    try:
                        if data is None:
                            portal.delete(full_name, name)
                        else:
                            portal.push(full_name, name, data)
                    except Exception as error:  # noqa: BLE001 - Preserve evidence and redact errors at boundaries.
                        errors.append(safe_error(error))
                if errors:
                    (args.output / "cleanup-errors.json").write_text(
                        json.dumps(errors, indent=2) + "\n"
                    )
                    raise RuntimeError(
                        "Could not restore all console controls; see cleanup-errors.json"
                    )
    print(f"Reports: {args.output / 'compat.md'} and {args.output / 'compat.json'}")
    return int(any(g.get("runner_error") or g["errors"] for g in results))


def interrupted(signum, frame):
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Compatibility run interrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as error:  # noqa: BLE001 - Preserve evidence and redact errors at boundaries.
        print(safe_error(error), file=sys.stderr)
        sys.exit(1)
