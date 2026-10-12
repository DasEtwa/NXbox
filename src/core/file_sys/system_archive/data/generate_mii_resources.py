# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate original NFTR/NFSR resources; consumes no external binary assets.

Layout reference: Vee's published ResourceShape/ResourceTexture structures and
nx_texture_mid_srgb.hexpat at commit 4ecb8ec3b49ec4f4f72a513a9a6bf052ed82deaf.
https://tangled.org/j0.lol/vee
"""

import math
import struct
import zlib
from pathlib import Path

SHAPE_COUNTS = (4, 12, 12, 132, 132, 132, 132, 132, 132, 1, 18, 18)
TEXTURE_COUNTS = (132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18)


def align(value, alignment=16):
    return (value + alignment - 1) // alignment * alignment


def mesh(kind):
    """Return our ellipsoid, front mask, hair cap, nose or accessory plane."""
    vertices, normals, uvs, indices = [], [], [], []
    if kind == "empty":
        return b"", (0,) * 5, (0,) * 5, 0, 0, (0.0,) * 6
    if kind in ("head", "hair", "mask"):
        rows, columns = 12, 16
        for row in range(rows + 1):
            v = row / rows
            # Avoid collapsed pole triangles, while retaining a closed stylized silhouette.
            theta = 0.02 + (math.pi - 0.04) * v
            if kind == "hair":
                theta = 0.02 + 1.35 * v
            for column in range(columns + 1):
                u = column / columns
                phi = 2 * math.pi * u
                if kind == "mask":
                    phi = math.pi * (0.15 + 0.7 * u)
                normal = (
                    math.sin(theta) * math.cos(phi),
                    math.cos(theta),
                    math.sin(theta) * math.sin(phi),
                )
                radii = (22, 30, 20) if kind == "head" else (23, 31, 21)
                vertices.append(tuple(n * r for n, r in zip(normal, radii)))
                normals.append(normal)
                uvs.append((u, v))
        for row in range(rows):
            for column in range(columns):
                a = row * (columns + 1) + column
                b = a + columns + 1
                indices.extend((a, a + 1, b, a + 1, b + 1, b))
    else:
        # A simple original rectangular accessory, or a pyramidal nose.
        vertices = [(-4, -3, 20), (4, -3, 20), (4, 3, 20), (-4, 3, 20)]
        indices = [0, 1, 2, 0, 2, 3]
        if kind == "nose":
            vertices.append((0, 0, 26))
            indices = [0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4]
        normals = [(0, 0, 1)] * len(vertices)
        uvs = [(0, 1), (1, 1), (1, 0), (0, 0), (0.5, 0.5)][: len(vertices)]
    buffers = [
        b"".join(struct.pack("<4e", *position, 1) for position in vertices),
        b"".join(
            struct.pack(
                "<I", sum((round(n * 511) & 1023) << (i * 10) for i, n in enumerate(normal))
            )
            for normal in normals
        ),
        b"".join(struct.pack("<2e", *uv) for uv in uvs),
        bytes((127, 0, 0, 0)) * len(vertices),
        bytes((255, 255, 255, 255)) * len(vertices),
    ]
    payload = bytearray()
    offsets = []
    for buffer in buffers:
        payload.extend(bytes(align(len(payload)) - len(payload)))
        offsets.append(len(payload))
        payload.extend(buffer)
    payload.extend(bytes(align(len(payload)) - len(payload)))
    index_offset = len(payload)
    payload.extend(struct.pack(f"<{len(indices)}H", *indices))
    bounds = tuple(min(p[i] for p in vertices) for i in range(3)) + tuple(
        max(p[i] for p in vertices) for i in range(3)
    )
    return bytes(payload), offsets, list(map(len, buffers)), index_offset, len(indices) * 2, bounds


def texture(kind):
    """Small original monochrome masks, with RGBA channels for SDK modulation."""
    size = 32
    pixels = bytearray()
    for y in range(size):
        for x in range(size):
            px, py = (x + 0.5) / size * 2 - 1, (y + 0.5) / size * 2 - 1
            opaque = False
            if kind == "eye":
                opaque = (px / 0.55) ** 2 + (py / 0.8) ** 2 < 1
            elif kind == "line":
                opaque = abs(py) < 0.12 and abs(px) < 0.8
            elif kind == "mouth":
                opaque = abs(py - 0.3 * (1 - px * px)) < 0.10 and abs(px) < 0.8
            elif kind == "dot":
                opaque = px * px + py * py < 0.16
            pixels.extend((255, 255, 255, 255 if opaque else 0))
    return bytes(pixels)


def common(offset, raw, compressed):
    return struct.pack("<IIIBBH", offset, len(raw), len(compressed), 6, 8, 0)


def make_shapes():
    header_size = 108 + sum(SHAPE_COUNTS) * 88 + 132 * 72 + 12 * 36
    out = bytearray(align(header_size))
    entries = bytearray()
    maxima = []
    kinds = (
        "empty",
        "head",
        "mask",
        "empty",
        "empty",
        "hair",
        "hair",
        "hair",
        "hair",
        "plane",
        "nose",
        "plane",
    )
    cache = {}
    for count, kind in zip(SHAPE_COUNTS, kinds):
        raw, offsets, sizes, index_offset, index_size, bounds = mesh(kind)
        if kind not in cache:
            compressed = zlib.compress(raw, 6)
            out.extend(bytes(align(len(out)) - len(out)))
            offset = len(out)
            out.extend(compressed)
            cache[kind] = common(offset, raw, compressed)
        element = cache[kind] + struct.pack(
            "<12I6f", *offsets, *sizes, index_offset, index_size, *bounds
        )
        entries.extend(element * count)
        maxima.append(len(raw))
    header = struct.pack("<4s26I", b"NFSR", 1, len(out), *maxima, *([16] * 12))
    out[:108] = header
    out[108 : 108 + len(entries)] = entries
    # Headwear and face translations/rotations are zero: our parts share an origin.
    return bytes(out)


def make_textures():
    header_size = 100 + sum(TEXTURE_COUNTS) * 28
    out = bytearray(align(header_size))
    entries = bytearray()
    cache = {}
    kinds = (
        "empty",
        "eye",
        "line",
        "empty",
        "empty",
        "empty",
        "empty",
        "dot",
        "mouth",
        "line",
        "line",
    )
    for count, kind in zip(TEXTURE_COUNTS, kinds):
        raw = texture(kind)
        if kind not in cache:
            compressed = zlib.compress(raw, 6)
            out.extend(bytes(align(len(out)) - len(out)))
            offset = len(out)
            out.extend(compressed)
            cache[kind] = common(offset, raw, compressed)
        entries.extend((cache[kind] + struct.pack("<IHH4B", 16, 32, 32, 2, 1, 1, 0)) * count)
    out[:100] = struct.pack("<4s24I", b"NFTR", 1, len(out), *([32 * 32 * 4] * 11), *([16] * 11))
    out[100 : 100 + len(entries)] = entries
    return bytes(out)


def generate_header():
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Generated by generate_mii_resources.py from original procedural artwork.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace FileSys::SystemArchive::MiiModelData {",
        "",
    ]
    for name, data in (("SHAPES", make_shapes()), ("TEXTURES", make_textures())):
        lines.append(f"inline constexpr std::array<std::uint8_t, {len(data)}> {name}{{{{")
        for start in range(0, len(data), 16):
            lines.append(
                "    " + ", ".join(f"0x{byte:02x}" for byte in data[start : start + 16]) + ","
            )
        lines.append("}};\n")
    lines.append("} // namespace FileSys::SystemArchive::MiiModelData\n")
    return "\n".join(lines)


if __name__ == "__main__":
    Path(__file__).with_name("mii_resources.h").write_text(generate_header())
