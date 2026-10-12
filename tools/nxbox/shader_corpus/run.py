#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay offline Eden guest environments through Maxwell -> IR -> HLSL."""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"yuzucach"
VERSIONS = {15: ("OpenGL", 624), 18: ("Vulkan", 1056)}
STAGES = ("vb", "tcs", "tes", "gs", "fs", "cs", "va")
TARGETS = ("vs", "hs", "ds", "gs", "ps", "cs", "vs")
HEADER = struct.Struct("<5Q7I")
MAX_CODE = 16 * 1024 * 1024
MAX_MAP = 1048576
SKIP = 77
RECORD = re.compile(
    r"^(va|vb|tcs|tes|gs|fs|cs) hash=([0-9a-fA-F]{16}) "
    r"address=(\d+) start=(\d+) bytes=(\d+)$"
)
LEGACY_RECORD = re.compile(
    r"^([0-9a-fA-F]{16}_s([0-5])_[0-9a-fA-F]{16}) "
    r"address=(\d+) start=(\d+) bytes=(\d+)$"
)


@dataclass
class Environment:
    stage: int
    data: bytes


@dataclass
class Case:
    name: str
    environments: list[Environment]
    reason: str = ""


class Reader:
    def __init__(self, stream):
        self.stream = stream

    def read(self, size):
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError(f"truncated input at byte {self.stream.tell()}")
        return data

    def u32(self):
        return struct.unpack("<I", self.read(4))[0]


def read_environment(reader):
    header = reader.read(HEADER.size)
    size, *fields = HEADER.unpack(header)
    counts = fields[:4]
    _local, _texture, start, base, highest, _viewport, stage = fields[4:]
    if stage >= len(STAGES):
        raise ValueError(f"invalid shader stage {stage}")
    if not size or size % 8 or size > MAX_CODE or base % 8 or start % 8:
        raise ValueError("invalid guest code length/alignment")
    if base + size > 1 << 32 or highest != base + size - 8:
        raise ValueError("invalid guest code address range")
    if any(count > MAX_MAP for count in counts):
        raise ValueError("environment map exceeds one million entries")
    tail_size = sum(count * width for count, width in zip(counts, (8, 8, 12, 12)))
    tail_size += 16 if stage == 5 else 80 + (32 if stage == 3 else 0)
    data = header + reader.read(size + tail_size)
    # Reject duplicate map entries rather than silently changing captured semantics.
    offset = HEADER.size + size
    for count, width, key_width in zip(counts, (8, 8, 12, 12), (4, 4, 8, 8)):
        keys = set()
        for _ in range(count):
            key = data[offset : offset + key_width]
            if key in keys:
                raise ValueError("duplicate serialized environment key")
            keys.add(key)
            offset += width
    return Environment(stage, data)


def cache_cases(path):
    """Read native little-endian GL v15 / Vulkan v18 pipeline cache records."""
    with path.open("rb") as stream:
        reader = Reader(stream)
        if reader.read(8) != MAGIC:
            raise ValueError(
                "not an Eden guest pipeline cache (driver binaries contain no guest code)"
            )
        version = reader.u32()
        if version not in VERSIONS:
            raise ValueError(f"unsupported Eden cache version {version}; supported: 15, 18")
        _, graphics_key_size = VERSIONS[version]
        index = 0
        while stream.peek(1):
            count = reader.u32()
            if not 1 <= count <= 6:
                raise ValueError(f"invalid pipeline environment count {count}")
            envs = [read_environment(reader) for _ in range(count)]
            stages = [env.stage for env in envs]
            if len(set(stages)) != count or (5 in stages and count != 1):
                raise ValueError("duplicate or mixed compute/graphics pipeline stages")
            # SerializePipeline writes sizeof(Key), even when Key::Size is shorter for hashing.
            reader.read(24 if stages == [5] else graphics_key_size)
            yield Case(f"{path}:pipeline{index}", envs)
            index += 1


def dump_environment(binary, stage, address, start, size):
    if size <= 0 or size % 8 or size > MAX_CODE:
        raise ValueError("invalid Maxwell dump size/alignment")
    if binary.stat().st_size != size:
        raise ValueError("manifest byte count mismatch")
    if address % 8 or start % 8 or address + size > 1 << 32:
        raise ValueError("invalid Maxwell dump address range")
    code = binary.read_bytes()
    if stage == 5:
        raise ValueError("compute dump lacks workgroup and shared-memory environment")
    offset = start - address
    if offset < 0 or offset + 80 > size:
        raise ValueError("program header is outside dump; capture did not retain header")
    sph = code[offset : offset + 80]
    # Read the SPH memory bitfields exactly as ProgramHeader::LocalMemorySize does.
    word0 = struct.unpack_from("<I", sph, 0)[0]
    word1 = struct.unpack_from("<I", sph, 4)[0]
    word2 = struct.unpack_from("<I", sph, 8)[0]
    word3 = struct.unpack_from("<I", sph, 12)[0]
    local = (word1 & 0xFFFFFF) | ((word2 & 0xFFFFFF) << 24)
    local += word3 & 0xFFFFFF
    if local > 0xFFFFFFFF:
        raise ValueError("dump local memory size overflows")
    # TextureBoundBuffer=2 follows GraphicsEnvironment's proprietary-driver default.
    # Uncaptured lookups are synthesized by the worker and reported as defaults.
    header = HEADER.pack(size, 0, 0, 0, 0, local, 2, start, address, address + size - 8, 1, stage)
    mask = bytes(32) if stage == 3 else b""
    if stage == 3 and (word0 >> 24) & 1:
        raise ValueError("geometry dump lacks passthrough mask environment")
    return Environment(stage, header + code + sph + mask)


def dump_cases(manifest):
    envs = []
    seen = set()
    lines = manifest.read_text().splitlines()
    if len(lines) < 2 or not re.fullmatch(r"pipeline=[0-9a-fA-F]{16}", lines[0]):
        raise ValueError("invalid pipeline manifest header")
    if lines[1] != "stages=va,vb,tcs,tes,gs,fs":
        raise ValueError("invalid pipeline manifest stage list")
    for line in lines[2:]:
        if not line:
            continue
        record = RECORD.fullmatch(line)
        legacy = LEGACY_RECORD.fullmatch(line)
        if record:
            name, _, address, start, size = record.groups()
            stage = STAGES.index(name)
        elif legacy:
            name, index, address, start, size = legacy.groups()
            stage = (6, 0, 1, 2, 3, 4)[int(index)]
            if not name.startswith(lines[0][9:] + "_s"):
                raise ValueError("dump record pipeline identity mismatch")
        else:
            raise ValueError(f"invalid stage record: {line}")
        if stage in seen:
            raise ValueError(f"duplicate dump stage {STAGES[stage]}")
        seen.add(stage)
        try:
            env = dump_environment(
                manifest.parent / (name + ".maxwell.bin"),
                stage,
                int(address),
                int(start),
                int(size),
            )
            envs.append(env)
        except (OSError, ValueError) as error:
            # Keep other stages runnable even if this stage is incomplete.
            yield Case(f"{manifest}:{STAGES[stage]}", [], str(error))
    if envs:
        yield Case(str(manifest), envs)
    elif not seen:
        raise ValueError("manifest contains no shader records")


def discover(directory):
    """Stream inputs, preserve complete cache entries before a corrupt tail."""
    paths = [directory] if directory.is_file() else sorted(directory.rglob("*"))
    for path in paths:
        if not path.is_file():
            continue
        try:
            if path.name == "pipeline.txt" or (
                path.suffix == ".txt" and re.fullmatch(r"[0-9a-fA-F]{16}", path.stem)
            ):
                yield from dump_cases(path)
                continue
            with path.open("rb") as stream:
                magic = stream.read(8)
            if magic == MAGIC or path.name in ("opengl.bin", "vulkan.bin"):
                yield from cache_cases(path)
            elif magic == b"yuzuvkch":
                yield Case(
                    str(path), [], "Vulkan driver cache contains no guest shader environments"
                )
        except (OSError, ValueError, struct.error) as error:
            yield Case(str(path), [], str(error))


def shader_jobs(case):
    by_stage = {env.stage: env for env in case.environments}
    for env in case.environments:
        if env.stage == 6 and 0 in by_stage:
            continue
        pair = by_stage.get(6) if env.stage == 0 else None
        yield env, pair


def failure_reason(output, returncode):
    failures = [
        line.removeprefix("FAIL: ") for line in output.splitlines() if line.startswith("FAIL: ")
    ]
    if failures:
        return failures[-1]
    if returncode < 0:
        return f"worker terminated by signal {-returncode}: {output.strip()}"
    return output.strip() or f"worker exited {returncode} without diagnostic"


def evaluate(
    directory, emitter, dxc=None, timeout=60, output=None, sm66=False, dxc_no_validation=False
):
    rows = []
    counts = Counter(
        total=0, emitted=0, emitted_with_defaults=0, failed=0, dxc_passed=0, dxc_failed=0
    )
    profiles = {}
    reasons = Counter()
    seen = Counter()
    opcodes = Counter()
    with tempfile.TemporaryDirectory(prefix="nxbox-corpus-") as temporary:
        work = Path(temporary)
        for case in discover(directory):
            if case.reason:
                counts["failed"] += 1
                counts["total"] += 1
                reasons[case.reason] += 1
                rows.append(
                    {"name": case.name, "stage": "input", "emitted": False, "reason": case.reason}
                )
                continue
            for env, pair in shader_jobs(case):
                counts["total"] += 1
                stage = STAGES[env.stage]
                seen[stage] += 1
                name = case.name + ":" + stage
                digest = hashlib.sha256(
                    name.encode() + env.data + (pair.data if pair else b"")
                ).hexdigest()
                source = work / (digest + ".hlsl")
                source.unlink(missing_ok=True)
                source.with_suffix(".dxil").unlink(missing_ok=True)
                if output:
                    (output / source.name).unlink(missing_ok=True)
                binary = work / "environment.bin"
                binary.write_bytes(env.data)
                command = [str(emitter), str(binary), str(source)]
                if pair:
                    va = work / "vertex-a.bin"
                    va.write_bytes(pair.data)
                    command.append(str(va))
                row = {"name": name, "stage": stage, "emitted": False, "reason": ""}
                try:
                    result = subprocess.run(
                        command, capture_output=True, text=True, timeout=timeout, check=False
                    )
                    row["diagnostics"] = result.stdout + result.stderr
                    if result.returncode:
                        raise ValueError(failure_reason(row["diagnostics"], result.returncode))
                    if not source.is_file() or not source.stat().st_size:
                        raise ValueError("worker returned success without HLSL source")
                    counts["emitted"] += 1
                    row["emitted"] = True
                    row["synthesized_keys"] = sorted(
                        set(
                            line.removeprefix("DEFAULT: ")
                            for line in result.stdout.splitlines()
                            if line.startswith("DEFAULT: ")
                        )
                    )
                    row["status"] = (
                        "emitted_with_defaults" if row["synthesized_keys"] else "emitted"
                    )
                    if row["synthesized_keys"]:
                        counts["emitted_with_defaults"] += 1
                    if output:
                        output.mkdir(parents=True, exist_ok=True)
                        artifact = output / source.name
                        shutil.copyfile(source, artifact)
                        row["source"] = str(artifact.resolve())
                    if dxc:
                        row["dxc_attempts"] = []
                        for model in ("6_0", "6_6") if sm66 else ("6_0",):
                            target = TARGETS[env.stage] + "_" + model
                            tally = profiles.setdefault(target, {"passed": 0, "failed": 0})
                            dxil = source.with_suffix("." + model + ".dxil")
                            dxil.unlink(missing_ok=True)
                            dxc_command = [str(dxc), "-T", target, "-E", "main", "-Fo", str(dxil)]
                            if dxc_no_validation:
                                dxc_command.append("-Vd")
                            dxc_command.append(str(source))
                            attempt = {"profile": target, "validation": not dxc_no_validation}
                            try:
                                compiled = subprocess.run(
                                    dxc_command,
                                    capture_output=True,
                                    text=True,
                                    timeout=timeout,
                                    check=False,
                                )
                                attempt["diagnostics"] = compiled.stdout + compiled.stderr
                                if (
                                    compiled.returncode
                                    or not dxil.is_file()
                                    or not dxil.stat().st_size
                                ):
                                    raise ValueError(
                                        failure_reason(attempt["diagnostics"], compiled.returncode)
                                    )
                                attempt["status"] = "passed"
                            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                                attempt["status"] = "failed"
                                attempt["reason"] = str(error)
                            tally[attempt["status"]] += 1
                            row["dxc_attempts"].append(attempt)
                        row["dxc_diagnostics"] = "\n".join(
                            attempt.get("diagnostics", attempt.get("reason", ""))
                            for attempt in row["dxc_attempts"]
                        )
                        failed = [a for a in row["dxc_attempts"] if a["status"] == "failed"]
                        if failed:
                            counts["dxc_failed"] += 1
                            row["dxc"] = "failed"
                            raise ValueError(
                                "DXC: "
                                + "\n".join(a["profile"] + ": " + a["reason"] for a in failed)
                            )
                        counts["dxc_passed"] += 1
                        row["dxc"] = "passed"
                except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                    if row["emitted"] and dxc and "dxc" not in row:
                        counts["dxc_failed"] += 1
                        row["dxc"] = "failed"
                    counts["failed"] += 1
                    row["reason"] = str(error)
                    reasons[row["reason"]] += 1
                    opcode = re.search(
                        r"(?:HLSL instruction Emit|opcode[ :=]+)([A-Za-z0-9_]+)", row["reason"]
                    )
                    if opcode:
                        row["opcode"] = opcode[1]
                        opcodes[opcode[1]] += 1
                rows.append(row)
    return {
        "counts": dict(counts),
        "stages": dict(seen),
        "dxc_profiles": profiles,
        "failures_by_reason": dict(reasons),
        "failures_by_opcode": dict(opcodes),
        "rows": rows,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--corpus",
        type=Path,
        default=Path(
            os.environ.get("NXBOX_SHADER_CORPUS", "~/.local/share/nxbox/corpus")
        ).expanduser(),
    )
    parser.add_argument(
        "--emitter", type=Path, default=os.environ.get("NXBOX_SHADER_CORPUS_EMITTER")
    )
    parser.add_argument(
        "--dxc", help="Optional DXC executable (DXC environment variable is also accepted)"
    )
    parser.add_argument(
        "--sm66", action="store_true", help="Also compile every emitted shader with SM 6.6"
    )
    parser.add_argument(
        "--dxc-no-validation",
        action="store_true",
        help="Pass -Vd for DXC installations without a validator; default validates",
    )
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument(
        "--report", type=Path, help="Write JSON with every result and exact failure reason"
    )
    parser.add_argument(
        "--output", type=Path, help="Retain HLSL locally; defaults to temporary files"
    )
    args = parser.parse_args(argv)
    if not args.corpus.exists():
        print(f"SKIP: corpus absent: {args.corpus}")
        return SKIP
    if not args.emitter or not args.emitter.is_file():
        parser.error("--emitter must name the built nxbox-shader-corpus executable")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    dxc_request = args.dxc or os.environ.get("DXC")
    dxc = shutil.which(dxc_request) if dxc_request else None
    if dxc_request and not dxc:
        parser.error(f"requested DXC is unavailable: {dxc_request}")
    emitter = args.emitter.resolve()
    report = evaluate(
        args.corpus, emitter, dxc, args.timeout, args.output, args.sm66, args.dxc_no_validation
    )
    counts = report["counts"]
    print("| total | emitted | failed | DXC passed | DXC failed | defaults |")
    print("| ---: | ---: | ---: | ---: | ---: | ---: |")
    print(
        "| "
        + " | ".join(
            str(counts[key])
            for key in (
                "total",
                "emitted",
                "failed",
                "dxc_passed",
                "dxc_failed",
                "emitted_with_defaults",
            )
        )
        + " |"
    )
    for opcode, count in sorted(report["failures_by_opcode"].items()):
        print(f"{count} failed opcode: {opcode}")
    for reason, count in sorted(report["failures_by_reason"].items()):
        print(f"{count} failed: {reason}")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    if not counts["total"]:
        print("SKIP: no recognized shader corpus inputs")
        return SKIP
    return int(counts["failed"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
