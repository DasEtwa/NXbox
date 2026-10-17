#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Translate offline Maxwell captures or synthetic IR and validate emitted DXIL."""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = ("vs", "vs", "hs", "ds", "gs", "ps")
SKIP = 77
RECORD = re.compile(
    r"^([0-9a-fA-F]{16}_s([0-5])_[0-9a-fA-F]{16}) "
    r"address=(\d+) start=(\d+) bytes=(\d+)$"
)


def dump_cases(directory):
    """Check exact GL manifest records, pair VA/VB, and retain pipeline identity."""
    cases = []
    for manifest in sorted(directory.glob("*.txt")):
        records = {}
        pipeline = manifest.stem
        for line in manifest.read_text().splitlines():
            if not line or line.startswith(("pipeline=", "stages=")):
                continue
            match = RECORD.fullmatch(line)
            if not match or not match[1].startswith(pipeline + "_s"):
                raise ValueError(f"{manifest}: invalid stage record: {line}")
            name, stage, address, start, size = match.groups()
            stage = int(stage)
            binary = directory / (name + ".maxwell.bin")
            if stage in records:
                raise ValueError(f"{manifest}: duplicate stage {stage}")
            if not binary.is_file() or binary.stat().st_size != int(size):
                raise ValueError(f"{binary}: missing binary or manifest byte count mismatch")
            if int(size) == 0 or int(size) % 8:
                raise ValueError(f"{binary}: expected nonempty little-endian 64-bit words")
            records[stage] = (name, binary, address, start)
        for stage, (name, binary, address, start) in sorted(records.items()):
            if stage == 0 and 1 in records:
                continue
            arguments = ["--dump", str(binary), str(stage), address, start]
            # The output path precedes optional VertexA arguments in the emitter CLI.
            vertex_a = []
            if stage == 1 and 0 in records:
                _, va_binary, va_address, va_start = records[0]
                vertex_a = [str(va_binary), va_address, va_start]
            cases.append((name, TARGETS[stage], arguments, vertex_a))
    if not cases:
        raise ValueError(f"{directory}: no shader manifest records found")
    return cases


def show_lines(source, diagnostics):
    """Show context for DXC's file:line:column diagnostics; fall back to full source."""
    lines = source.read_text().splitlines()
    locations = [int(n) for n in re.findall(r":(\d+):\d+:\s*(?:error|warning)", diagnostics)]
    selected = set()
    for location in locations:
        selected.update(range(max(1, location - 2), min(len(lines), location + 2) + 1))
    if not selected:
        selected.update(range(1, len(lines) + 1))
    for number in sorted(selected):
        print(f"{number:5} | {lines[number - 1]}")


def validate(args):
    if not (ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl.h").is_file():
        print("SKIP: src/shader_recompiler/backend/hlsl is not available yet")
        return SKIP
    dxc = shutil.which(args.dxc)
    if not dxc:
        print(f"SKIP: DXC unavailable ({args.dxc}); run tools/hlsl-harness/fetch_dxc.py")
        return SKIP
    emitter = args.emitter.resolve()
    if not emitter.is_file():
        print(f"FAIL: emitter not built: {emitter}; see tools/hlsl-harness/README.md")
        return 1
    probe = subprocess.run([str(emitter), "--probe"], check=False, capture_output=True, text=True)
    if probe.returncode:
        print(probe.stdout + probe.stderr, end="")
        return probe.returncode
    try:
        cases = (
            dump_cases(args.dumps)
            if args.dumps
            else [
                ("synthetic_" + stage, stage, ["--synthetic", stage], [])
                for stage in ("vs", "ps", "cs")
            ]
        )
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}")
        return 1
    args.output.mkdir(parents=True, exist_ok=True)
    failures = 0
    for name, stage, arguments, vertex_a in cases:
        source = args.output / (name + ".hlsl")
        dxil = source.with_suffix(".dxil")
        # A previous successful artifact must never survive a failed rerun.
        source.unlink(missing_ok=True)
        dxil.unlink(missing_ok=True)
        try:
            emitted = subprocess.run(
                [str(emitter), *arguments, str(source), *vertex_a],
                check=False,
                capture_output=True,
                text=True,
                timeout=args.timeout,
            )
            if emitted.returncode:
                print(f"FAIL {name}: frontend/EmitHLSL\n{emitted.stdout}{emitted.stderr}")
                failures += 1
                continue
            compiled = subprocess.run(
                [dxc, "-T", stage + "_6_0", "-E", "main", "-Fo", str(dxil), str(source)],
                check=False,
                capture_output=True,
                text=True,
                timeout=args.timeout,
            )
            diagnostics = compiled.stdout + compiled.stderr
            source.with_suffix(".dxc.log").write_text(diagnostics)
            if compiled.returncode or not dxil.is_file() or dxil.stat().st_size == 0:
                print(f"FAIL {name} ({stage}_6_0)\n{diagnostics}")
                show_lines(source, diagnostics)
                dxil.unlink(missing_ok=True)
                failures += 1
            else:
                print(f"PASS {name} ({stage}_6_0)")
                if diagnostics:
                    print(diagnostics, end="")
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"FAIL {name}: {error}")
            failures += 1
    print(f"{len(cases) - failures} passed, {failures} failed")
    return int(failures != 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dumps", type=Path, help="LocalState/shader_dump directory (default: synthetic IR)"
    )
    parser.add_argument("--dxc", default=os.environ.get("DXC", "dxc"))
    parser.add_argument("--emitter", type=Path, default=ROOT / "build-hlsl-harness/hlsl-emitter")
    parser.add_argument("--output", type=Path, default=ROOT / "build-hlsl-harness/results")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    return validate(args)


if __name__ == "__main__":
    sys.exit(main())
