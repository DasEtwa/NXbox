#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare per-pass raw render targets using only the Python standard library."""

import argparse
import json
import math
import struct
from collections import defaultdict, deque
from pathlib import Path


def unsigned_float(bits, mantissa_bits):
    """Decode a signless five-bit-exponent float (R11/G11/B10)."""
    exponent, mantissa = bits >> mantissa_bits, bits & ((1 << mantissa_bits) - 1)
    if exponent == 31:
        return math.nan if mantissa else math.inf
    if exponent == 0:
        return math.ldexp(mantissa, -14 - mantissa_bits)
    return math.ldexp(1 + mantissa / (1 << mantissa_bits), exponent - 15)


def decode_raw(data, format_name, width, height):
    """Return RGB tuples; missing G/B channels are zero and alpha is ignored.

    SRGB bytes are expanded to linear RGB. Packed values and scalar floats use
    the guest's little-endian representation.
    """
    layouts = {
        "A8B8G8R8_UNORM": "4B",
        "A8B8G8R8_SRGB": "4B",
        "B8G8R8A8_UNORM": "4B",
        "B8G8R8A8_SRGB": "4B",
        "A2B10G10R10_UNORM": "I",
        "R16G16B16A16_FLOAT": "4e",
        "R32G32B32A32_FLOAT": "4f",
        "B10G11R11_FLOAT": "I",
        "R16G16_FLOAT": "2e",
        "R8_UNORM": "B",
        "R16_FLOAT": "e",
        "R32_FLOAT": "f",
    }
    if format_name not in layouts:
        raise ValueError(f"unsupported format: {format_name}")
    layout = "<" + layouts[format_name]
    if width <= 0 or height <= 0 or len(data) != width * height * struct.calcsize(layout):
        raise ValueError("Raw byte count does not match dimensions and format")
    result = []
    for values in struct.iter_unpack(layout, data):
        if format_name.startswith(("A8B8G8R8", "B8G8R8A8")):
            rgb = values[:3]
            if format_name.startswith("B8G8R8A8"):
                rgb = rgb[::-1]
            rgb = tuple(v / 255 for v in rgb)
            if format_name.endswith("SRGB"):
                rgb = tuple(
                    v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in rgb
                )
        elif format_name == "A2B10G10R10_UNORM":
            rgb = tuple(((values[0] >> shift) & 1023) / 1023 for shift in (0, 10, 20))
        elif format_name == "B10G11R11_FLOAT":
            packed = values[0]
            rgb = (
                unsigned_float(packed & 2047, 6),
                unsigned_float((packed >> 11) & 2047, 6),
                unsigned_float(packed >> 22, 5),
            )
        elif format_name == "R8_UNORM":
            rgb = (values[0] / 255, 0.0, 0.0)
        else:
            rgb = tuple(values[:3]) + (0.0,) * max(0, 3 - len(values))
        result.append(rgb)
    return result


def pair_passes(a, b):
    """Match each key's k-th occurrence in sequence order, preserving A order."""

    def key(entry):
        address = entry["gpu_addr"]
        if isinstance(address, str):
            address = int(address, 16 if address.lower().startswith("0x") else 10)
        return (address, entry["width"], entry["height"], entry["format"], entry["slot"])

    pending = defaultdict(deque)
    for entry in sorted(b, key=lambda item: item["seq"]):
        pending[key(entry)].append(entry)
    pairs, unmatched_a = [], []
    for entry in sorted(a, key=lambda item: (item["seq"], item["slot"])):
        matches = pending[key(entry)]
        if matches:
            pairs.append((entry, matches.popleft()))
        else:
            unmatched_a.append(entry)
    unmatched_b = sorted(
        (entry for entries in pending.values() for entry in entries),
        key=lambda item: (item["seq"], item["slot"]),
    )
    return pairs, unmatched_a, unmatched_b


def clamp(value):
    if math.isnan(value):
        raise ValueError("NaN channel in raw image")
    return max(0.0, min(1.0, value))


def compare(a, b):
    if not a or len(a) != len(b):
        raise ValueError("Pixel counts differ or images are empty")
    squared, maximum = 0.0, 0.0
    for left, right in zip(a, b):
        for x, y in zip(left, right):
            # Max error retains HDR differences even when PSNR clips them away.
            if math.isnan(x) or math.isnan(y):
                raise ValueError("NaN channel in raw image")
            error = 0.0 if x == y else abs(x - y)
            maximum = max(maximum, error)
            squared += (clamp(x) - clamp(y)) ** 2
    mse = squared / (3 * len(a))
    return (math.inf if mse == 0 else -10 * math.log10(mse)), maximum


def write_preview(path, width, height, a, b):
    def pixel(rgb):
        return bytes(round(clamp(v) * 255) for v in rgb)

    raster = bytearray()
    for row in range(height):
        start, end = row * width, (row + 1) * width
        for image in (a, b):
            for rgb in image[start:end]:
                raster.extend(pixel(rgb))
        for left, right in zip(a[start:end], b[start:end]):
            raster.extend(pixel(tuple(abs(clamp(x) - clamp(y)) for x, y in zip(left, right))))
    path.write_bytes(f"P6\n{width * 3} {height}\n255\n".encode("ascii") + raster)


def read_manifest(directory):
    with (directory / "manifest.jsonl").open(encoding="utf-8") as source:
        entries = [json.loads(line) for line in source if line.strip()]
        return [entry for entry in entries if "event" not in entry]


def run(dir_a, dir_b, out, threshold=40, report=print):
    pairs, unmatched_a, unmatched_b = pair_passes(read_manifest(dir_a), read_manifest(dir_b))
    for label, entries in (("A", unmatched_a), ("B", unmatched_b)):
        for entry in entries:
            report(f"UNMATCHED {label}: seq={entry['seq']} file={entry['file']}")
    first = None
    for a, b in pairs:
        prefix = f"seq={a['seq']} file={a['file']} B_seq={b['seq']}"
        if a.get("skipped") or b.get("skipped"):
            report(f"{prefix}: skipped A={a.get('skipped', '-')} B={b.get('skipped', '-')}")
            continue
        try:
            left = decode_raw(
                (dir_a / a["file"]).read_bytes(), a["format"], a["width"], a["height"]
            )
            right = decode_raw(
                (dir_b / b["file"]).read_bytes(), b["format"], b["width"], b["height"]
            )
            psnr, maximum = compare(left, right)
            report(f"{prefix}: PSNR={psnr:.4f} dB max_abs_error={maximum:.6g}")
            if psnr < threshold:
                if first is None:
                    first = a
                out.mkdir(parents=True, exist_ok=True)
                write_preview(
                    out / f"pass_{a['seq']:04}_c{a['slot']}.ppm",
                    a["width"],
                    a["height"],
                    left,
                    right,
                )
        except (OSError, ValueError) as error:
            report(f"{prefix}: {error}")
    report(
        f"FIRST DIVERGENT: seq={first['seq']} file={first['file']}" if first else "NO DIVERGENCE"
    )
    return first


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dir_a", type=Path)
    parser.add_argument("dir_b", type=Path)
    parser.add_argument("--out", type=Path, default=Path("pass_diff_report"))
    parser.add_argument("--threshold", type=float, default=40)
    args = parser.parse_args()
    if not math.isfinite(args.threshold):
        parser.error("threshold must be finite")
    try:
        run(args.dir_a, args.dir_b, args.out, args.threshold)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
