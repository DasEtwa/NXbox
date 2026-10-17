# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unattended Xbox title x renderer matrix; dry-run never contacts a console."""

import argparse
import hashlib
import io
import itertools
import json
import math
import re
import signal
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from .diag import FIELDS, fatal_lines, parse_diag
from .images import contact_sheet, png
from .portal import Portal, PortalError, credentials
from .run import ConsoleLock, interrupted, run_game, safe_error

UTC = timezone.utc  # noqa: UP017 - The port suite also runs on system Python 3.9.


ARTIFACTS = Path.home() / ".local/share/nxbox/artifacts"
CONTROL_FILES = ("nxbox_env.txt", "game.txt", "game.url", "skip_library.txt", "eden_settings.txt")


def generic_input(seconds):
    events = []
    for start in range(5, math.ceil(seconds), 8):
        events.extend((f"{start}:A:0.3", f"{start + 2}:PLUS:0.3"))
        if start >= 45:
            events.append(f"{start + 3}:LSTICK_RIGHT:3")
    return ",".join(events)


def inventory(portal, package):
    """Include uncached packages rather than silently excluding unknown title IDs."""

    def listing(directory):
        try:
            return portal.json("/api/filesystem/apps/files", portal.files(package, directory)).get(
                "Items", []
            )
        except PortalError as error:
            if error.status == 404:
                return []
            raise

    games = {}
    warnings = []
    for item in listing("LocalState/library"):
        name = item.get("Name", "")
        if not re.fullmatch(r"[0-9a-fA-F]{16}\.json", name):
            continue
        try:
            record = json.loads(portal.pull(package, name, "LocalState/library"))
            path = record["path"]
            title = record["title_id"].upper()
            if not re.fullmatch(r"[0-9A-F]{16}", title) or not isinstance(path, str):
                raise ValueError("Invalid library record")
            if any(c in path for c in "\n\r\0") or ".." in path.replace("\\", "/").split("/"):
                raise ValueError("Invalid library path")
            key = path.replace("\\", "/").lower()
            games[key] = {
                "title_id": title,
                "file_name": path,
                "name": record.get("name", path),
                "source": "library",
                "content_type": record.get("type", record.get("content_type", "base")),
            }
        except (ValueError, KeyError, TypeError):
            warnings.append(f"Invalid library record: {name}")
    local_names = set()
    for item in listing("LocalState/games"):
        name = item.get("Name", "")
        if not re.fullmatch(r"[^/\\\r\n\0]+\.(?:nsp|xci|nsz)", name, re.IGNORECASE):
            continue
        local_names.add(name.lower())
        matches = [
            g
            for g in games.values()
            if g["file_name"].replace("\\", "/").split("/")[-1].lower() == name.lower()
        ]
        if matches:
            continue
        match = re.search(r"\[([0-9a-fA-F]{16})\]", name)
        title = (
            match[1].upper()
            if match
            else "unknown-" + hashlib.sha256(name.encode()).hexdigest()[:16]
        )
        games["games/" + name.lower()] = {
            "title_id": title,
            "file_name": "games\\" + name,
            "name": name,
            "source": "directory",
        }
    # Cached local paths can outlive removed games. External cache records are retained,
    # with accessibility determined by the launch diagnostics.
    result = []
    for game in games.values():
        path = game["file_name"].replace("\\", "/").lower()
        if "/games/" in path or path.startswith("games/"):
            if path.split("/")[-1] not in local_names:
                warnings.append(f"Stale or external library path: {game['title_id']}")
                # Drive-qualified cache paths may point to removable storage.
                if not path.startswith("games/"):
                    result.append(game)
                continue
        else:
            result.append(game)
            continue
        result.append(game)
    return sorted(result, key=lambda g: (g["title_id"], g["file_name"])), warnings


def group_titles(packages):
    """Only identified base applications launch; preserve all other inventory."""
    groups = {}
    for package in packages:
        title = package["title_id"]
        if not re.fullmatch(r"[0-9A-F]{16}", title):
            base = title
            kind = "unknown"
        else:
            value = int(title, 16)
            # Add-on IDs occupy the next 0x1000 range; patches use base + 0x800.
            kind = str(
                package.get("content_type", "base" if title.endswith("000") else "additional")
            ).lower()
            is_dlc = kind in ("dlc", "addon", "add-on", "add_on_content") or value & 0xFFF not in (
                0,
                0x800,
            )
            base = f"{(value - 0x1000 if is_dlc else value) & ~0xFFF:016X}"
        launchable = title.endswith("000") and kind in ("base", "application", "program", "0")
        group = groups.setdefault(
            base, {"title_id": base, "base_packages": [], "additional_content": []}
        )
        group["base_packages" if launchable else "additional_content"].append(package)
    games = []
    for group in groups.values():
        for base in group["base_packages"]:
            games.append(dict(base, additional_content=group["additional_content"]))
    return games, list(groups.values())


def parse_evidence(text, crash=""):
    result = parse_diag(text)
    result["fps_per_5s"] = [dict(s, fps=s["frames"] / s["seconds"]) for s in result["presents"]]
    result["thread_profile"] = [
        line for line in text.splitlines() if re.search(r"\b(?:NXBOX_)?THREAD_PROFILE\b", line)
    ]
    result["failure_lines"] = [
        line
        for line in (text + "\n" + crash).splitlines()
        if fatal_lines(line) or re.search(r"what\(\)|\brenderer_exception\s+what=", line)
    ]
    counts = {}
    for line in text.splitlines():
        if "InfoQueue" in line:
            match = re.search(r"severity=(\w+)", line)
            if match:
                severity = match[1].lower()
                counts[severity] = counts.get(severity, 0) + 1
    result["infoqueue_counts"] = counts
    result["infoqueue_available"] = "InfoQueue" in text
    result["infoqueue_count_basis"] = "observed severity message lines; duplicates retained"
    result["fatal_lines"] = fatal_lines(text + "\n" + crash)
    result["fatal"] = bool(result["fatal_lines"])
    result["draws_per_frame"] = [
        s.get("draws_per_frame", s.get("draws", 0) / s["frames"])
        for s in result["presents"]
        if s["frames"] > 0 and ("draws" in s or "draws_per_frame" in s)
    ]
    for line in text.splitlines():
        if re.search(r"\b(?:NXBOX_)?D3D12_FRAME_PROFILE\b", line):
            fields = {key: float(value) for key, value in FIELDS.findall(line)}
            frames = fields.get("frames", fields.get("presents", 0))
            draws = fields.get("draws", fields.get("draw_calls"))
            if frames > 0 and draws is not None and draws >= 0:
                result["draws_per_frame"].append(draws / frames)
    return result


def classify(result, changes):
    if result.get("runner_error"):
        return "error", "Runner failure; game state unknown"
    if result.get("fatal") or result.get("new_dumps") or result.get("process_disappeared"):
        return "crashed", "Fatal log evidence, new dump, or process exited before deadline"
    active = [s["fps"] for s in result["fps_per_5s"] if s["fps"] > 0]
    if not active:
        return "boot", "No positive presentation interval"
    black = result.get("black_frames", [])
    if len(black) >= 2 and all(black[1:]):
        return "black-screen", "Presents observed; all post-launch captures are near black"
    draws = result.get("draws_per_frame", [])
    draw_evidence = not draws or sum(draws) / len(draws) >= 10
    if (
        draw_evidence
        and len(active) >= 3
        and sum(c >= 0.08 for c in changes) >= 2
        and sum(active) / len(active) >= 10
    ):
        return (
            "in-game",
            "Heuristic: sustained >=10 FPS, >=10 draws/frame when available and two changing captures (animated menus can match)",
        )
    return "menus", "Presentation observed; insufficient evidence of sustained scene changes"


def require_pillow():
    from PIL import Image

    return Image


def frame_signature(data):
    from PIL import Image

    with Image.open(io.BytesIO(data)) as image:
        return image.convert("RGB").resize((32, 18)).tobytes()


def frame_change(before, after):
    return sum(abs(a - b) for a, b in zip(before, after)) / (len(before) * 255)


def near_black(signature):
    return bool(signature) and sum(value <= 12 for value in signature) / len(signature) >= 0.99


def env_assignment(value):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=[^\r\n\0]*", value):
        raise argparse.ArgumentTypeError("Expected a single-line KEY=VALUE")
    return value


def case_environment(renderer, common=(), d3d12=()):
    values = {"NXBOX_THREAD_PROFILE": "1"}
    if renderer == "d3d12":
        values.update(NXBOX_D3D12_DEBUG="1", NXBOX_D3D12_FRAME_PROFILE="1")
    for assignment in (*common, *(d3d12 if renderer == "d3d12" else ())):
        key, value = assignment.split("=", 1)
        values[key] = value
    values["NXBOX_RENDERER"] = renderer
    return "\n".join(f"{key}={value}" for key, value in values.items())


def disable_crash_collection(portal, full, warnings):
    try:
        portal.request("DELETE", "/api/debug/dump/usermode/crashcontrol", {"packageFullName": full})
    except PortalError as error:
        warnings.append("Crash dump collection could not be disabled: " + safe_error(error))


def package_identity(path):
    with zipfile.ZipFile(path) as archive:
        manifest = ET.fromstring(archive.read("AppxManifest.xml"))
    identity = next((node for node in manifest if node.tag.rsplit("}", 1)[-1] == "Identity"), None)
    if identity is None:
        raise ValueError("Package manifest has no Identity")
    name, version, architecture = (
        identity.attrib[key] for key in ("Name", "Version", "ProcessorArchitecture")
    )
    if name != "NSPX.NXbox" or not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", version):
        raise ValueError("Expected an NXbox main package")
    return f"{name}_{version}_{architecture}_"


def installed_package(portal, prefix):
    packages = portal.json("/api/app/packagemanager/packages").get("InstalledPackages", [])
    matches = [p for p in packages if p["PackageFullName"].startswith(prefix)]
    if len(matches) != 1:
        raise RuntimeError("Supplied package identity not uniquely installed")
    return matches[0]


def install(portal, paths, timeout=900):
    boundary = hashlib.sha256(str(time.time_ns()).encode()).hexdigest()
    body = bytearray()
    for path in paths:
        name = path.name
        if any(c in name for c in '\r\n"') or not path.is_file() or path.stat().st_size == 0:
            raise ValueError("Missing, empty or invalid package file")
        body.extend(
            f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; filename="{name}"\r\nContent-Type: application/octet-stream\r\n\r\n'.encode()
        )
        body.extend(path.read_bytes())
        body.extend(b"\r\n")
    body.extend(f"--{boundary}--\r\n".encode())
    deadline = time.monotonic() + timeout
    while True:
        try:
            portal.request(
                "POST",
                "/api/app/packagemanager/package",
                {"package": paths[0].name},
                bytes(body),
                f"multipart/form-data; boundary={boundary}",
            )
            break
        except PortalError as error:
            if error.status != 409 or time.monotonic() >= deadline:
                raise
            time.sleep(5)
    started = time.monotonic()
    idle = 0
    while time.monotonic() < deadline:
        time.sleep(2)
        raw = portal.request("GET", "/api/app/packagemanager/state")
        state = json.loads(raw) if raw.strip() else {}
        code = int(state.get("Code", 0))
        if code not in (0, 3805991009):
            raise RuntimeError(f"Package installation failed code={code}")
        idle = idle + 1 if code == 0 else 0
        if idle >= 2 and time.monotonic() - started > 8:
            return
    raise TimeoutError("Package installation timed out")


def previous_run(root, dry_run):
    for path in sorted(
        root.glob("matrix-*/compat.json"), key=lambda p: p.stat().st_mtime, reverse=True
    ):
        try:
            report = json.loads(path.read_text())
            if not isinstance(report, dict):
                continue
            if (
                report.get("schema_version") == 2
                and report.get("dry_run") == dry_run
                and isinstance(report.get("games"), list)
                and report["games"]
            ):
                return path, report
        except (ValueError, OSError):
            continue
    return None, {}


def matrix_diff(results, previous):
    old = {(g["case_id"], g["renderer"]): g for g in previous.get("games", [])}
    ranks = {"crashed": 0, "error": 0, "boot": 1, "black-screen": 1, "menus": 2, "in-game": 3}
    changes = []
    for game in results:
        before = old.get((game["case_id"], game["renderer"]))
        if before is None:
            changes.append(
                {
                    "case_id": game["case_id"],
                    "renderer": game["renderer"],
                    "change": "new",
                    "regression": False,
                }
            )
            continue
        old_fps = before.get("mean_fps", 0)
        regression = (
            ranks[game["state"]] < ranks[before["state"]]
            or (old_fps > 0 and game["mean_fps"] < old_fps * 0.8)
            or sum(game["infoqueue_counts"].values())
            > sum(before.get("infoqueue_counts", {}).values())
        )
        changes.append(
            {
                "case_id": game["case_id"],
                "renderer": game["renderer"],
                "before": before["state"],
                "after": game["state"],
                "fps_before": old_fps,
                "fps_after": game["mean_fps"],
                "regression": regression,
            }
        )
    current = {(g["case_id"], g["renderer"]) for g in results}
    for key in old.keys() - current:
        changes.append(
            {"case_id": key[0], "renderer": key[1], "change": "missing", "regression": False}
        )
    return changes


def write_reports(output, report):
    (output / "compat.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "diff.json").write_text(json.dumps(report["diff"], indent=2) + "\n")
    esc = lambda s: str(s).replace("|", "\\|").replace("\n", " ").replace("\r", " ")
    lines = [
        "# NXbox compatibility matrix",
        "",
        f"Build: `{report['sha']}`; dry-run: {report['dry_run']}",
        "",
        "States are heuristics, not proof of correctness. FPS comes from guest GAME_PRESENT intervals; absent instrumentation is unknown.",
        "",
        "| Title | Renderer | State | Mean FPS | InfoQueue |",
        "| --- | --- | --- | --- | --- |",
    ]
    for game in report["games"]:
        lines.append(
            f"| {esc(game['name'])} | {game['renderer']} | {game['state']} | {game['mean_fps']:.2f} | {sum(game['infoqueue_counts'].values())} observed |"
        )
        paths = [output / p for p in game["frames"]]
        contact_sheet(paths, output / game["case_id"] / game["renderer"] / "contact-sheet.png")
    overview = [output / game["frames"][-1] for game in report["games"] if game["frames"]]
    contact_sheet(overview, output / "contact-sheet.png")
    lines.extend(
        [
            "",
            "![Matrix contact sheet](contact-sheet.png)",
            "",
            "Overview tiles follow table order, omitting cases without captures.",
        ]
    )
    lines.extend(
        [
            "",
            "## Difference from previous matrix",
            "",
            f"Previous: {esc(report['previous'] or 'none')}",
            "",
        ]
    )
    for change in report["diff"]:
        lines.append(
            f"- {'**REGRESSION**' if change['regression'] else 'Change'}: `{change['case_id']}` / {change['renderer']}: {esc(json.dumps(change))}"
        )
    lines.extend(["", "## Installed content by base title", ""])
    for group in report.get("inventory", []):
        lines.append(f"- `{group['title_id']}`: {len(group['base_packages'])} base package(s)")
        for content in group["additional_content"]:
            lines.append(f"  - `{esc(content['title_id'])}`: {esc(content['name'])} (not launched)")
    for game in report["games"]:
        prefix = f"{game['case_id']}/{game['renderer']}"
        lines.extend(
            [
                "",
                f"## {esc(game['name'])} / {game['renderer']}",
                "",
                game["state_reason"],
                "",
                f"[Diagnostics]({prefix}/eden_uwp_diag.txt) · [Eden log]({prefix}/eden_log.txt)",
                "",
                f"![Contact sheet]({prefix}/contact-sheet.png)",
                "",
                f"Profile lines: {len(game['thread_profile'])}; failure lines: {len(game['failure_lines'])}.",
            ]
        )
        lines.extend(
            f"- {esc(line)}"
            for line in game["failure_lines"] + game.get("errors", []) + game.get("warnings", [])
        )
    lines.extend(
        ["", "## Inventory / cleanup notes", ""] + [f"- {esc(w)}" for w in report["warnings"]]
    )
    (output / "compat.md").write_text("\n".join(lines) + "\n")


def run_case(portal, package, game, output, renderer, seconds, environment=None):
    from PIL import Image

    case = dict(game, title_id=game["case_id"])
    root = output / game["case_id"]
    root.mkdir(exist_ok=True)
    # run_game creates its own leaf; reverse the directory roles for stable links.
    run = dict(case, title_id=renderer)
    snapshots = []
    signatures = []

    def snapshot(elapsed, folder):
        data = portal.request("GET", "/ext/screenshot")
        number = len(snapshots)
        name = f"shot-{number:04d}"
        (folder / (name + ".png")).write_bytes(data)
        with Image.open(io.BytesIO(data)) as image:
            image.convert("RGB").save(folder / (name + ".ppm"))
        signatures.append(frame_signature(data))
        snapshots.append({"seconds": elapsed, "path": f"{game['case_id']}/{renderer}/{name}.png"})

    result = run_game(
        portal,
        package,
        run,
        root,
        seconds,
        5,
        environment if environment is not None else case_environment(renderer),
        generic_input(seconds),
        1,
        snapshot=snapshot,
        monitor=True,
    )
    folder = root / renderer
    text = "\n".join(
        (folder / n).read_text(errors="replace") if (folder / n).exists() else ""
        for n in ("eden_uwp_diag.txt", "eden_log.txt")
    )
    crash = folder / "nxbox_crash.txt"
    result.update(parse_evidence(text, crash.read_text(errors="replace") if crash.exists() else ""))
    result["event_log_file"] = (
        f"{game['case_id']}/{renderer}/nxbox_crash.txt" if crash.exists() else None
    )
    result["crash_file"] = (
        result["event_log_file"]
        if crash.exists() and fatal_lines(crash.read_text(errors="replace"))
        else None
    )
    result["black_frames"] = [near_black(s) for s in signatures]
    changes = [frame_change(a, b) for a, b in itertools.pairwise(signatures)]
    result["state"], result["state_reason"] = classify(result, changes)
    result.update(
        game,
        renderer=renderer,
        frames=[f"{game['case_id']}/{renderer}/shot-{i:04d}.ppm" for i in range(len(snapshots))],
        screenshots=snapshots,
        frame_changes=changes,
    )
    samples = result["fps_per_5s"]
    result["mean_fps"] = (
        sum(s["frames"] for s in samples) / sum(s["seconds"] for s in samples) if samples else 0
    )
    return result


def dry_case(game, output, renderer):
    folder = output / game["case_id"] / renderer
    folder.mkdir(parents=True)
    text = (
        "GAME_RUNNING\nGAME_FIRST_FRAME\n"
        + "GAME_PRESENT frames=150 seconds=5\n" * 3
        + "THREAD_PROFILE tid=7 name=GPU cpu_percent=25 interval_ms=5000\nD3D12 InfoQueue severity=WARNING id=1 synthetic\n"
    )
    (folder / "eden_uwp_diag.txt").write_text(text)
    (folder / "eden_log.txt").write_text("Synthetic dry-run evidence; no console was contacted.\n")
    frames = []
    for i, color in enumerate((b"\0\0\0", b"\xff\0\0", b"\0\xff\0")):
        path = folder / f"shot-{i:04d}.ppm"
        path.write_bytes(b"P6\n1 1\n255\n" + color)
        frames.append(str(path.relative_to(output)))
        path.with_suffix(".png").write_bytes(png(1, 1, color))
    result = dict(
        game,
        **parse_evidence(text),
        renderer=renderer,
        frames=frames,
        errors=[],
        mean_fps=30,
        frame_changes=[1 / 3, 2 / 3],
        screenshots=[
            {"seconds": i * 30, "path": str((folder / f"shot-{i:04d}.png").relative_to(output))}
            for i in range(3)
        ],
    )
    result["state"], result["state_reason"] = classify(result, result["frame_changes"])
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sha", required=True, help="Commit SHA of the supplied main package")
    parser.add_argument("--appx", type=Path, help="Main build package; required for live runs")
    parser.add_argument("--dependency", type=Path, action="append", default=[])
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument(
        "--renderers", nargs="+", choices=("opengl", "d3d12"), default=["opengl", "d3d12"]
    )
    parser.add_argument("--artifacts", type=Path, default=ARTIFACTS)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--env", action="append", type=env_assignment, default=[])
    parser.add_argument("--env-d3d12", action="append", type=env_assignment, default=[])
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-fA-F]{7,40}", args.sha) or not 0 < args.seconds <= 86400:
        parser.error("sha must be 7..40 hex digits; seconds must be 0..86400")
    if not args.dry_run and (not args.appx or not args.appx.is_file()):
        parser.error("live runs require --appx")
    if len(set(args.renderers)) != len(args.renderers):
        parser.error("renderers must be unique")
    if not args.dry_run:
        require_pillow()  # Fail before touching the console if unavailable.

        prefix = package_identity(args.appx)
    args.artifacts.mkdir(parents=True, exist_ok=True)
    previous, old = previous_run(args.artifacts, args.dry_run)
    output = args.artifacts / f"matrix-{args.sha}-{datetime.now(UTC).strftime('%Y%m%dT%H%M%S%fZ')}"
    output.mkdir()
    report = {
        "schema_version": 2,
        "sha": args.sha,
        "dry_run": args.dry_run,
        "previous": str(previous) if previous else None,
        "games": [],
        "inventory": [],
        "warnings": [],
        "diff": [],
        "complete": False,
        "duration_seconds": args.seconds,
    }

    def execute(games, portal=None, package=None):
        games, report["inventory"] = group_titles(games)
        if not games:
            raise RuntimeError("No installed game packages discovered")
        for game in games:
            game["case_id"] = (
                game["title_id"] + "-" + hashlib.sha256(game["file_name"].encode()).hexdigest()[:8]
            )
            for renderer in args.renderers:
                print(f"{game['title_id']} / {renderer}", flush=True)
                result = (
                    dry_case(game, output, renderer)
                    if args.dry_run
                    else run_case(
                        portal,
                        package,
                        game,
                        output,
                        renderer,
                        args.seconds,
                        case_environment(renderer, args.env, args.env_d3d12),
                    )
                )
                report["games"].append(result)
                report["diff"] = matrix_diff(report["games"], old)
                write_reports(output, report)
                if result.get("stop_failed"):
                    raise RuntimeError("Stop failed; refusing to launch another title")

    try:
        if args.dry_run:
            execute(
                [
                    {
                        "title_id": "0100000000000000",
                        "file_name": "synthetic.nsp",
                        "name": "Synthetic fixture",
                    }
                ]
            )
        else:
            with ConsoleLock():
                portal = Portal(*credentials())
                for p in portal.json("/api/app/packagemanager/packages").get(
                    "InstalledPackages", []
                ):
                    if p["PackageFullName"].startswith("NSPX.NXbox_"):
                        portal.stop(p["PackageFullName"])
                install(portal, [args.appx, *args.dependency])
                package = installed_package(portal, prefix)
                full = package["PackageFullName"]
                report["package"] = full
                report["package_sha256"] = hashlib.sha256(args.appx.read_bytes()).hexdigest()
                games, report["warnings"] = inventory(portal, full)
                saved = {
                    (name, "LocalState"): portal.pull(full, name, optional=True)
                    for name in CONTROL_FILES
                }
                # Per-title overrides are applied after the global settings file.
                try:
                    items = portal.json(
                        "/api/filesystem/apps/files", portal.files(full, "LocalState/settings")
                    )
                except PortalError as error:
                    if error.status != 404:
                        raise
                    items = {"Items": []}
                for item in items.get("Items", []):
                    name = item.get("Name", "")
                    if re.fullmatch(r"[0-9a-fA-F]{16}\.txt", name):
                        saved[name, "LocalState/settings"] = portal.pull(
                            full, name, "LocalState/settings"
                        )
                controls = output / "previous-controls"
                controls.mkdir()
                manifest = []
                for (name, directory), data in saved.items():
                    key = directory.replace("/", "-") + "-" + name
                    manifest.append(
                        {
                            "name": name,
                            "directory": directory,
                            "backup": key if data is not None else None,
                        }
                    )
                    if data is not None:
                        (controls / key).write_bytes(data)
                (controls / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
                try:
                    # Disable full crash dumps; leave stored dumps intact.
                    disable_crash_collection(portal, full, report["warnings"])
                    portal.delete(full, "game.url")
                    # Explicit OpenGL global setting: NXBOX_RENDERER only forces D3D12.
                    for (name, directory), data in saved.items():
                        if name == "eden_settings.txt" or directory == "LocalState/settings":
                            settings = (data or b"").decode("utf-8", "replace")
                            settings = re.sub(
                                r"(?m)^\s*(?:renderer_backend|backend)\s*=.*$", "", settings
                            )
                            portal.push(
                                full,
                                name,
                                (settings + "\nrenderer_backend=4\n").encode(),
                                directory,
                            )
                    execute(games, portal, package)
                finally:
                    cleanup = []
                    try:
                        portal.stop(full)
                    except Exception as error:  # noqa: BLE001 - Preserve evidence and attempt every cleanup.
                        cleanup.append(safe_error(error))
                    for (name, directory), data in saved.items():
                        try:
                            if data is None:
                                portal.delete(full, name, directory)
                            else:
                                portal.push(full, name, data, directory)
                        except Exception as error:  # noqa: BLE001 - Preserve evidence and attempt every cleanup.
                            cleanup.append(f"{name}: {safe_error(error)}")
                    report["warnings"].extend(cleanup)
                    if cleanup:
                        raise RuntimeError("Console cleanup failed; see compat.json")
        report["complete"] = True
    except KeyboardInterrupt:
        report["warnings"].append("Interrupted; matrix incomplete")
        raise
    except Exception as error:
        report["warnings"].append(safe_error(error))
        raise
    finally:
        write_reports(output, report)
        print(f"Reports: {output}", flush=True)
    return int(any(g.get("runner_error") or g.get("errors") for g in report["games"]))


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as error:  # noqa: BLE001 - Preserve evidence and attempt every cleanup.
        print(safe_error(error), file=sys.stderr)
        sys.exit(1)
