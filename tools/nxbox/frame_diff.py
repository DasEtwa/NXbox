#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare matching binary RGB PPM captures without third-party dependencies."""

import argparse
import math
from pathlib import Path


def read_ppm(path):
    data = path.read_bytes()
    offset = 0

    def token():
        nonlocal offset
        while offset < len(data):
            if data[offset] in b" \t\r\n\v\f":
                offset += 1
            elif data[offset] == ord("#"):
                end = data.find(b"\n", offset)
                offset = len(data) if end < 0 else end + 1
            else:
                break
        start = offset
        while offset < len(data) and data[offset] not in b" \t\r\n\v\f#":
            offset += 1
        if start == offset:
            raise ValueError("Incomplete PPM header")
        return data[start:offset]

    magic = token()
    width, height, maximum = int(token()), int(token()), int(token())
    if magic != b"P6" or maximum != 255 or width <= 0 or height <= 0:
        raise ValueError("Expected a P6 PPM with positive dimensions and max value 255")
    if offset >= len(data) or data[offset] not in b" \t\r\n\v\f":
        raise ValueError("Missing PPM raster separator")
    # Consume only the header separator: raster bytes may themselves be whitespace.
    offset += 2 if data[offset : offset + 2] == b"\r\n" else 1
    pixels = data[offset:]
    if len(pixels) != width * height * 3:
        raise ValueError("PPM raster size does not match its dimensions")
    return width, height, pixels


def compare(a, b):
    squared_error = 0
    differing = 0
    heatmap = bytearray(len(a))
    for offset in range(0, len(a), 3):
        differences = [a[offset + c] - b[offset + c] for c in range(3)]
        squared_error += sum(value * value for value in differences)
        if any(abs(value) > 16 for value in differences):
            differing += 1
            heatmap[offset : offset + 3] = b"\xff\x00\x00"
        else:
            heatmap[offset : offset + 3] = bytes(a[offset + c] // 4 for c in range(3))
    mse = squared_error / len(a)
    psnr = math.inf if mse == 0 else 10 * math.log10(255 * 255 / mse)
    percent = 100 * differing / (len(a) // 3)
    return psnr, percent, heatmap


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dir_a", type=Path)
    parser.add_argument("dir_b", type=Path)
    parser.add_argument("--out", type=Path, default=Path("frame_diff_report"))
    args = parser.parse_args()
    lines = []

    def report(line):
        print(line)
        lines.append(line)

    try:
        args.out.mkdir(parents=True, exist_ok=True)
        if not args.dir_a.is_dir() or not args.dir_b.is_dir():
            raise ValueError("Both input directories must exist")
        names_a = {path.name for path in args.dir_a.glob("*.ppm") if path.is_file()}
        names_b = {path.name for path in args.dir_b.glob("*.ppm") if path.is_file()}
        for name in sorted(names_a ^ names_b):
            report(f"{name}: missing from {'B' if name in names_a else 'A'}")
        for name in sorted(names_a & names_b):
            try:
                width, height, a = read_ppm(args.dir_a / name)
                other_width, other_height, b = read_ppm(args.dir_b / name)
                if (width, height) != (other_width, other_height):
                    raise ValueError("Frame dimensions differ")
                psnr, percent, heatmap = compare(a, b)
                report(f"{name}: PSNR={psnr:.4f} dB, pixels >16={percent:.4f}%")
                (args.out / f"diff_{name}").write_bytes(
                    f"P6\n{width} {height}\n255\n".encode("ascii") + heatmap
                )
            except (OSError, ValueError) as error:
                report(f"{name}: error: {error}")
        if not names_a & names_b:
            report("No matching PPM frames")
    except (OSError, ValueError) as error:
        report(f"Error: {error}")
    try:
        (args.out / "report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    except OSError as error:
        print(f"Error writing report: {error}")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        # This diagnostic tool always exits successfully, including argument errors.
        pass
