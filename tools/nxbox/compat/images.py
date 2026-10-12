"""Read binary RGB PPM and write PNG/contact sheets without dependencies."""

import math
import re
import struct
import zlib


def read_ppm(data):
    position = 0
    tokens = []
    for _ in range(4):
        while position < len(data):
            if data[position : position + 1] == b"#":
                end = data.find(b"\n", position)
                if end < 0:
                    raise ValueError("Unterminated PPM comment")
                position = end + 1
            elif data[position : position + 1].isspace():
                position += 1
            else:
                break
        match = re.match(rb"[^\s#]+", data[position:])
        if not match:
            raise ValueError("Incomplete PPM header")
        tokens.append(match.group())
        position += len(match.group())
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError("Expected 8-bit P6 RGB PPM")
    width, height = int(tokens[1]), int(tokens[2])
    if not (0 < width <= 8192 and 0 < height <= 8192):
        raise ValueError("Invalid PPM dimensions")
    if not data[position : position + 1].isspace():
        raise ValueError("Missing PPM raster separator")
    position += 2 if data[position : position + 2] == b"\r\n" else 1
    pixels = data[position:]
    if len(pixels) != width * height * 3:
        raise ValueError("Invalid PPM raster size")
    return width, height, pixels


def png(width, height, pixels):
    def chunk(kind, payload):
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    stride = width * 3
    if len(pixels) != stride * height:
        raise ValueError("Invalid RGB raster size")
    rows = b"".join(b"\0" + pixels[y * stride : (y + 1) * stride] for y in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def contact_sheet(paths, destination, columns=4, tile_width=320, tile_height=180):
    width = columns * tile_width
    height = max(1, math.ceil(len(paths) / columns)) * tile_height
    canvas = bytearray(width * height * 3)
    # Empty runs still produce a valid black PNG; the report explains missing evidence.
    for index, path in enumerate(paths):
        sw, sh, pixels = read_ppm(path.read_bytes())
        scale = min(tile_width / sw, tile_height / sh)
        tw, th = max(1, int(sw * scale)), max(1, int(sh * scale))
        left = index % columns * tile_width + (tile_width - tw) // 2
        top = index // columns * tile_height + (tile_height - th) // 2
        for y in range(th):
            for x in range(tw):
                source = (min(sh - 1, int(y / scale)) * sw + min(sw - 1, int(x / scale))) * 3
                target = ((top + y) * width + left + x) * 3
                canvas[target : target + 3] = pixels[source : source + 3]
    destination.write_bytes(png(width, height, canvas))
