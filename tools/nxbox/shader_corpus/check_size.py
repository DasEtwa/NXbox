#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure a corpus replay and compare every baseline program through glslang/SPIR-V.

Artifacts remain in --output. The baseline report pairs jobs by name; distinct baseline
HLSL contents are checked once, while all jobs contribute to the size distribution.
"""

import argparse
import concurrent.futures
import difflib
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess

STAGES = {"vb": "vert", "fs": "frag", "cs": "comp", "gs": "geom"}


def canonical_ids(text):
    """Normalize IDs and declaration order; retain every instruction and decoration.

    Local results keep their execution order. Global IDs derive from their defining
    types/constants and resource decorations, so moving an unchanged declaration
    cannot create a false difference. Signed and unsigned constants remain distinct.
    """
    lines = text.splitlines()
    defs = {}
    decorations = {}
    locals = {}
    inside = False
    function_count = 0
    for line in lines:
        if line.startswith(("OpDecorate ", "OpMemberDecorate ")):
            m = re.match(r"\w+ (%\d+) (.*)", line)
            decorations.setdefault(m[1], []).append(m[2])
        m = re.match(r"(%\d+) = (.*)", line)
        if not m:
            continue
        key, expr = m.groups()
        if expr.startswith("OpFunction "):
            inside = True
            locals[key] = f"%function{function_count}"
            function_count += 1
        elif inside:
            locals[key] = f"%local{len(locals)}"
        else:
            defs[key] = expr
    memo = dict(locals)

    def resolve(key):
        if key in memo:
            return memo[key]
        expr = defs[key]
        expr = re.sub(r"%\d+", lambda m: resolve(m[0]), expr)
        if expr.startswith("OpVariable"):
            expr += " " + str(sorted(decorations.get(key, [])))
        memo[key] = "%g" + hashlib.sha256(expr.encode()).hexdigest()
        return memo[key]

    out = []
    global_lines = []
    annotation_lines = []
    inside = False
    for line in lines:
        line = re.sub(r"%\d+", lambda m: resolve(m[0]), line)
        if " = OpFunction " in line:
            inside = True
        if inside:
            out.append(line)
        elif line.startswith(("OpDecorate ", "OpMemberDecorate ")):
            annotation_lines.append(line)
        elif re.match(r"%\w+ = ", line):
            global_lines.append(line)
        else:
            out.append(line)
    return "\n".join(sorted(annotation_lines) + sorted(global_lines) + out) + "\n"


def compile_shader(source, stage, directory):
    directory.mkdir(parents=True, exist_ok=True)
    assembly = directory / "optimized.txt"
    status = directory / "status.json"
    if status.exists():
        cached = json.loads(status.read_text())
        if not cached["ok"] or cached.get("canonical_version") == 2:
            return cached
        process = subprocess.run(
            [
                "spirv-dis",
                "--no-header",
                "--raw-id",
                "--no-indent",
                str(directory / "optimized.spv"),
            ],
            capture_output=True,
            text=True,
            check=True,
            timeout=120,
        )
        assembly.write_text(canonical_ids(process.stdout))
        cached["canonical_version"] = 2
        status.write_text(json.dumps(cached))
        return cached
    commands = [
        [
            "glslangValidator",
            "-D",
            "-e",
            "main",
            "-S",
            STAGES[stage],
            "-V",
            "-o",
            str(directory / "raw.spv"),
            str(source),
        ],
        [
            "spirv-opt",
            "-O",
            "--strip-debug",
            "--strip-reflect",
            str(directory / "raw.spv"),
            "-o",
            str(directory / "optimized.spv"),
        ],
        ["spirv-dis", "--no-header", "--raw-id", "--no-indent", str(directory / "optimized.spv")],
    ]
    result = {}
    for index, command in enumerate(commands):
        process = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if process.returncode:
            result = {"ok": False, "phase": index, "diagnostics": process.stdout + process.stderr}
            break
        if index == 2:
            assembly.write_text(canonical_ids(process.stdout))
            result = {"ok": True, "canonical_version": 2}
    status.write_text(json.dumps(result))
    return result


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True, help="Baseline replay JSON report")
    parser.add_argument("--report", type=Path, required=True, help="Candidate replay JSON report")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-cache", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    baseline = json.loads(args.baseline.read_text())["rows"]
    candidate = {row["name"]: row for row in json.loads(args.report.read_text())["rows"]}
    sizes = []
    unique = {}
    for row in baseline:
        other = candidate[row["name"]]
        if not row["emitted"]:
            continue
        if not other["emitted"]:
            raise RuntimeError(f"Lost baseline emission: {row['name']}")
        source = Path(row["source"])
        digest = hashlib.sha1(source.read_bytes()).hexdigest()
        size = Path(other["source"]).stat().st_size
        sizes.append(size)
        # The same baseline shader must receive the same candidate output in every job.
        candidate_digest = hashlib.sha1(Path(other["source"]).read_bytes()).hexdigest()
        if digest in unique and unique[digest]["candidate_digest"] != candidate_digest:
            raise RuntimeError(f"Inconsistent emission: {row['name']}")
        unique.setdefault(
            digest,
            {
                "id": digest[:12],
                "source": str(source),
                "candidate": other["source"],
                "candidate_digest": candidate_digest,
                "stage": row["stage"],
                "baseline_bytes": source.stat().st_size,
                "bytes": size,
            },
        )

    def check(item):
        row = dict(item)
        base_dir = args.baseline_cache / row["id"]
        new_dir = args.output / row["id"]
        row["baseline_check"] = compile_shader(row["source"], row["stage"], base_dir)
        # Compile candidates even when the baseline fails, to retain complete diagnostics.
        row["candidate_check"] = compile_shader(row["candidate"], row["stage"], new_dir)
        if row["baseline_check"]["ok"] and row["candidate_check"]["ok"]:
            before = (base_dir / "optimized.txt").read_text()
            after = (new_dir / "optimized.txt").read_text()
            row["identical"] = before == after
            if before != after:
                (new_dir / "difference.diff").write_text(
                    "".join(
                        difflib.unified_diff(
                            before.splitlines(True),
                            after.splitlines(True),
                            fromfile="baseline",
                            tofile="candidate",
                        )
                    )
                )
        return row

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        rows = list(executor.map(check, unique.values()))
    base_ok = [row for row in rows if row["baseline_check"]["ok"]]
    summary = {
        "jobs": len(sizes),
        "distinct": len(rows),
        "total_bytes": sum(sizes),
        "median": statistics.median(sizes),
        "p90": percentile(sizes, 0.9),
        "distinct_bytes": sum(row["bytes"] for row in rows),
        "distinct_median": statistics.median(row["bytes"] for row in rows),
        "distinct_p90": percentile([row["bytes"] for row in rows], 0.9),
        "baseline_compiled": len(base_ok),
        "regressions": sum(not row["candidate_check"]["ok"] for row in base_ok),
        "identical": sum(row.get("identical", False) for row in base_ok),
        "different": sum(row.get("identical") is False for row in base_ok),
        "top20": sorted(rows, key=lambda row: -row["baseline_bytes"])[:20],
    }
    (args.output / "results.json").write_text(
        json.dumps({"summary": summary, "rows": rows}, indent=2)
    )
    print(json.dumps({key: value for key, value in summary.items() if key != "top20"}, indent=2))
    return bool(summary["regressions"])


if __name__ == "__main__":
    raise SystemExit(main())
