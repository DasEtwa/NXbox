#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch official DXC for this host, or build a pinned native compiler."""

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

REPOSITORY = "https://github.com/microsoft/DirectXShaderCompiler"
TAG = "v1.9.2609"


def request(url):
    return urllib.request.urlopen(
        urllib.request.Request(url, headers={"User-Agent": "NXbox-HLSL-harness"}), timeout=60
    )


def check_dxc(executable):
    subprocess.run([str(executable), "--version"], check=True)
    # Exercise native DXIL generation, not just loading the driver executable.
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "smoke.hlsl"
        output = source.with_suffix(".dxil")
        source.write_text("[numthreads(1,1,1)] void main() {}\n")
        subprocess.run(
            [str(executable), "-T", "cs_6_0", "-E", "main", "-Fo", str(output), str(source)],
            check=True,
        )
        if not output.is_file() or output.stat().st_size == 0:
            raise RuntimeError("DXC did not produce DXIL")
    print(f"DXC={executable.resolve()}")


def unpack(archive, destination):
    """Reject escaping paths and archive links before extracting release files."""
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                if (
                    not (destination / member.filename)
                    .resolve()
                    .is_relative_to(destination.resolve())
                ):
                    raise RuntimeError("unsafe archive path")
                if member.external_attr >> 16 & 0o170000 == 0o120000:
                    raise RuntimeError("archive symlink is not allowed")
            bundle.extractall(destination)
    else:
        with tarfile.open(archive) as bundle:
            for member in bundle.getmembers():
                if not (destination / member.name).resolve().is_relative_to(destination.resolve()):
                    raise RuntimeError("unsafe archive path")
                if not member.isfile() and not member.isdir():
                    raise RuntimeError("archive link or special file is not allowed")
            bundle.extractall(destination, filter="data")


def build_source(destination, jobs):
    source = destination / "source"
    build = destination / "build"
    if not source.exists():
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", TAG, REPOSITORY, str(source)], check=True
        )
    revision = subprocess.check_output(
        ["git", "-C", str(source), "describe", "--tags", "--exact-match"], text=True
    ).strip()
    if revision != TAG:
        raise RuntimeError(f"source checkout is not pinned to {TAG}")
    subprocess.run(
        ["git", "-C", str(source), "submodule", "update", "--init", "--recursive"], check=True
    )
    subprocess.run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-C",
            str(source / "cmake/caches/PredefinedParams.cmake"),
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DLLVM_INCLUDE_TESTS=OFF",
            "-DHLSL_INCLUDE_TESTS=OFF",
            "-DENABLE_SPIRV_CODEGEN=OFF",
            "-DLLVM_ENABLE_ZLIB=OFF",
        ],
        check=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build), "--target", "dxc", "--parallel", str(jobs)], check=True
    )
    # Keep build/lib beside build/bin: DXC's native loader uses that layout.
    return build / "bin/dxc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=Path(".cache/hlsl-harness/dxc"))
    parser.add_argument(
        "--source", action="store_true", help="build native DXC instead of fetching a release"
    )
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    if args.source:
        executable = build_source(destination, args.jobs)
    else:
        system = platform.system().lower()
        machine = platform.machine().lower()
        if system not in ("linux", "darwin"):
            raise RuntimeError("this fetcher supports macOS and Linux; use --source on other hosts")
        with request(
            f"https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/tags/{TAG}"
        ) as response:
            release = json.load(response)
        names = ("mac", "darwin", "osx") if system == "darwin" else ("linux",)
        arm = machine in ("arm64", "aarch64")
        candidates = [
            a
            for a in release["assets"]
            if any(n in a["name"].lower() for n in names)
            and a["name"].endswith((".tar.gz", ".zip"))
            and (
                any(n in a["name"].lower() for n in ("arm64", "aarch64"))
                if arm
                else any(n in a["name"].lower() for n in ("x64", "x86_64", "x86_x64"))
            )
        ]
        if not candidates:
            print(
                f"No official {system}/{machine} binary in {TAG}; building native DXC.", flush=True
            )
            executable = build_source(destination, args.jobs)
        else:
            if len(candidates) != 1:
                raise RuntimeError("ambiguous release assets; use --source")
            asset = candidates[0]
            with tempfile.TemporaryDirectory() as directory:
                archive = Path(directory) / asset["name"]
                with (
                    request(asset["browser_download_url"]) as response,
                    archive.open("wb") as output,
                ):
                    shutil.copyfileobj(response, output)
                digest = asset.get("digest")
                if not digest or not digest.startswith("sha256:"):
                    raise RuntimeError("release asset lacks a SHA-256 digest; use --source")
                with archive.open("rb") as downloaded:
                    actual = hashlib.file_digest(downloaded, "sha256").hexdigest()
                if actual != digest.removeprefix("sha256:"):
                    raise RuntimeError("release SHA-256 mismatch")
                unpack(archive, destination)
            executable = destination / "bin/dxc"
            executable.chmod(executable.stat().st_mode | 0o111)
    check_dxc(executable)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
