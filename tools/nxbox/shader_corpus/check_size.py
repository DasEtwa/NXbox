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
import re
import statistics
import subprocess
from pathlib import Path

from spirv_audit import graph, phi_normalized_graph

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
    source_digest = hashlib.sha256(Path(source).read_bytes()).hexdigest()
    cached = json.loads(status.read_text()) if status.exists() else None
    if cached and cached.get("source_sha256") == source_digest:
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
        (directory / "disassembly.txt").write_text(process.stdout)
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
        process = subprocess.run(command, check=False, capture_output=True, text=True, timeout=120)
        if process.returncode:
            result = {"ok": False, "phase": index, "diagnostics": process.stdout + process.stderr}
            break
        if index == 2:
            (directory / "disassembly.txt").write_text(process.stdout)
            assembly.write_text(canonical_ids(process.stdout))
            result = {"ok": True, "canonical_version": 2}
    result["source_sha256"] = source_digest
    status.write_text(json.dumps(result))
    return result


def disassembly(directory):
    path = directory / "disassembly.txt"
    if not path.exists():
        process = subprocess.run(
            [
                "spirv-dis",
                "--no-header",
                "--raw-id",
                "--no-indent",
                str(directory / "optimized.spv"),
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )
        path.write_text(process.stdout)
    return path.read_text()


def audit_difference(before, after):
    """Classify a difference; never treat an unproved graph change as equivalent."""
    try:
        if graph(before) == graph(after):
            return "value_numbering_or_schedule"
        if graph(before, indices=True) == graph(after, indices=True):
            return "positive_address_index_signedness"
        if graph(before, commute=True, indices=True) == graph(after, commute=True, indices=True):
            return "commutative_operands_and_numbering"
        if phi_normalized_graph(before, commute=True, indices=True) == phi_normalized_graph(
            after, commute=True, indices=True
        ):
            return "phi_value_numbering_and_commutative_operands"
        return "unexplained"
    except (KeyError, ValueError, RecursionError) as error:
        return "unexplained: " + str(error)


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
    parser.add_argument("--previous-check", type=Path)
    parser.add_argument("--require-identical", action="store_true")
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
                row["audit"] = audit_difference(disassembly(base_dir), disassembly(new_dir))
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
        if args.previous_check and row["candidate_check"]["ok"]:
            previous = args.previous_check / row["id"]
            if (previous / "optimized.txt").exists():
                row["incremental_identical"] = (previous / "optimized.txt").read_text() == (
                    new_dir / "optimized.txt"
                ).read_text()
                if not row["incremental_identical"]:
                    row["incremental_audit"] = audit_difference(
                        disassembly(previous), disassembly(new_dir)
                    )
        return row

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        rows = list(executor.map(check, unique.values()))
    base_ok = [row for row in rows if row["baseline_check"]["ok"]]
    base_compiled = [row for row in rows if row["baseline_check"].get("phase") != 0]
    summary = {
        "jobs": len(sizes),
        "distinct": len(rows),
        "total_bytes": sum(sizes),
        "median": statistics.median(sizes),
        "p90": percentile(sizes, 0.9),
        "distinct_bytes": sum(row["bytes"] for row in rows),
        "distinct_median": statistics.median(row["bytes"] for row in rows),
        "distinct_p90": percentile([row["bytes"] for row in rows], 0.9),
        "baseline_compiled": len(base_compiled),
        "baseline_optimized": len(base_ok),
        "compile_regressions": sum(
            row["candidate_check"].get("phase") == 0 for row in base_compiled
        ),
        "regressions": sum(not row["candidate_check"]["ok"] for row in base_ok),
        "identical": sum(row.get("identical", False) for row in base_ok),
        "different": sum(row.get("identical") is False for row in base_ok),
        "audits": {
            reason: sum(row.get("audit") == reason for row in base_ok)
            for reason in sorted({row["audit"] for row in base_ok if "audit" in row})
        },
        "incremental_identical": sum(row.get("incremental_identical", False) for row in base_ok),
        "incremental_audits": {
            reason: sum(row.get("incremental_audit") == reason for row in base_ok)
            for reason in sorted(
                {row["incremental_audit"] for row in base_ok if "incremental_audit" in row}
            )
        },
        "top20": sorted(rows, key=lambda row: -row["baseline_bytes"])[:20],
    }
    (args.output / "results.json").write_text(
        json.dumps({"summary": summary, "rows": rows}, indent=2)
    )
    print(json.dumps({key: value for key, value in summary.items() if key != "top20"}, indent=2))
    audits_to_gate = summary["incremental_audits"] if args.previous_check else summary["audits"]
    identical = summary["incremental_identical"] if args.previous_check else summary["identical"]
    return bool(
        summary["regressions"]
        or summary["compile_regressions"]
        or any(reason.startswith("unexplained") for reason in audits_to_gate)
        or (args.require_identical and identical != len(base_ok))
    )


if __name__ == "__main__":
    raise SystemExit(main())
