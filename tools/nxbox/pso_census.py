#!/usr/bin/env python3
"""Read-only census of version-3 NXbox graphics records (standard library only)."""

import argparse
import collections
import hashlib
import pathlib
import struct

STATE_SIZE = 656
SCALARS = [
    "topology_type",
    "cull_mode",
    "fill_mode",
    "front_ccw",
    "cull_all",
    "depth_clip",
    "depth_enable",
    "depth_write",
    "depth_func",
    "stencil_enable",
    "stencil_read_mask",
    "stencil_write_mask",
    "independent_blend",
    "num_render_targets",
    "sample_count",
    "patch_vertices",
    "logic_enable",
    "logic_op",
    "alpha_to_coverage",
    "conservative_raster",
    "smooth_lines",
    "depth_bounds",
    "stencil_back_read_mask",
    "stencil_back_write_mask",
]
FIELDS = [(name, i, 1) for i, name in enumerate(SCALARS)]
FIELDS += [
    (name, 24 + i * 4, 4)
    for i, name in enumerate(
        ("depth_bias", "slope_scaled_bias_bits", "bias_clamp_bits", "dsv_format")
    )
]
FIELDS += [(f"rtv[{i}].format", 40 + i * 4, 4) for i in range(8)]
FIELDS += [
    (f"{face}.{name}", 72 + f * 4 + i, 1)
    for f, face in enumerate(("front", "back"))
    for i, name in enumerate(("fail", "depth_fail", "pass", "func"))
]
FIELDS += [
    (f"blend[{t}].{name}", 80 + t * 8 + i, 1)
    for t in range(8)
    for i, name in enumerate(
        ("enable", "write_mask", "src", "dst", "op", "src_alpha", "dst_alpha", "op_alpha")
    )
]
FIELDS += [
    (f"attribute[{t}].{name}", 144 + t * 16 + off, size)
    for t in range(32)
    for name, off, size in (
        ("enabled", 0, 1),
        ("slot", 1, 1),
        ("offset", 2, 2),
        ("dxgi", 4, 4),
        ("instanced", 8, 1),
        ("swap_rb", 9, 1),
        ("scaled", 10, 1),
        ("pad", 11, 1),
        ("divisor", 12, 4),
    )
]


def fnv(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def normalize(state):
    """Mirror NormalizeGraphicsState; preserve binding metadata in attributes."""
    s = bytearray(state)
    if not s[6]:
        s[7:9] = bytes((0, 8))
    if not s[9]:
        s[10:12] = bytes(2)
        s[22:24] = bytes(2)
        s[72:80] = bytes((1, 1, 1, 8)) * 2
    if not (s[9] & 2):
        s[22:24] = bytes(2)
    if s[0] != 4:
        s[15] = 0
    for i in range(32):
        off = 144 + i * 16
        if not s[off + 8]:
            s[off + 12 : off + 16] = bytes(4)
    if not s[16]:
        s[17] = 4
    for i in range(8):
        off = 80 + i * 8
        if (s[12] or i == 0) and (i < s[13]) and struct.unpack_from("<I", s, 40 + i * 4)[0]:
            if not s[off]:
                s[off + 2 : off + 8] = bytes((2, 1, 1, 2, 1, 1))
        elif not s[12] and i:
            s[off : off + 8] = s[80:88]
        elif s[12]:
            s[off : off + 8] = bytes((0, 0, 2, 1, 1, 2, 1, 1))
    return bytes(s)


def feedback_metadata(runtime):
    """Skip the five serialized Shader::Info blocks; return the SO declaration."""
    pos = 0
    for _ in range(5):
        (present,) = struct.unpack_from("<I", runtime, pos)
        pos += 4
        if present not in (0, 1):
            raise ValueError("invalid metadata boolean")
        if not present:
            continue
        # mask + 18 CBUF sizes + 8 fragment exports + 32 interpolation modes
        # + 512 pairs of load/store flags, all serialized as U32.
        pos += (1 + 18 + 8 + 32 + 512 * 2) * 4
        for width in (2, 4, 9, 8, 13, 9):
            (count,) = struct.unpack_from("<I", runtime, pos)
            if count > 1024:
                raise ValueError("invalid descriptor count")
            pos += 4 + count * width * 4
    (count,) = struct.unpack_from("<I", runtime, pos)
    expected = 4 + (256 * 20 if count else 0)
    if count > 256 or len(runtime) - pos != expected:
        raise ValueError("invalid stream output metadata")
    return runtime[pos:]


def read_record(path):
    b = path.read_bytes()
    if len(b) < 100 or b[:8] != b"NXD3D12C" or struct.unpack_from("<I", b, 8)[0] != 3:
        raise ValueError(f"{path}: unsupported disk header")
    if struct.unpack_from("<I", b, 56)[0] != 2:
        raise ValueError(f"{path}: not graphics")
    sizes = struct.unpack_from("<3Q", b, 68)
    if sum(sizes) + 100 != len(b) or fnv(b[:-8]) != struct.unpack_from("<Q", b, len(b) - 8)[0]:
        raise ValueError(f"{path}: size/checksum mismatch")
    payload, request, _runtime = sizes
    p = memoryview(b)[92 : 92 + payload]
    if struct.unpack_from("<I", p)[0] != 4 or request != 704:
        raise ValueError(f"{path}: unsupported recipe/request schema")
    state = bytes(p[12 : 12 + STATE_SIZE])
    pos = 12 + STATE_SIZE
    (count,) = struct.unpack_from("<I", p, pos)
    pos += 4
    stages = []
    sources = []
    for _ in range(count):
        stage, hash_value, cbv, srv, uav, sampler, size = struct.unpack_from("<IQ5I", p, pos)
        pos += 32
        stages.append((stage, hash_value))
        sources.append(
            (stage, cbv, srv, uav, sampler, hashlib.sha256(p[pos : pos + size]).digest())
        )
        pos += size
    if pos != len(p):
        raise ValueError(f"{path}: trailing recipe")
    r = b[92 + payload : 92 + payload + request]
    metadata = b[92 + payload + request : -8]
    feedback = feedback_metadata(metadata) if metadata else b""
    return {
        "state": state,
        "stages": tuple(stages),
        "sources": tuple(sources),
        "stage_recipe": hashlib.sha256(p[12 + STATE_SIZE :]).digest(),
        "feedback": feedback,
        "topology": bytes(p[8:12]),
        "root": bytes(p[4:8]),
        "request": r,
        "runtime": b[92 + payload + request : -8],
    }


def census(folder, verbose=True):
    records = [read_record(p) for p in sorted(folder.glob("*.graphics"))]
    groups = collections.defaultdict(list)
    for r in records:
        r["normalized"] = normalize(r["state"])
        groups[r["stages"]].append(r)
    distribution = sorted(collections.Counter(map(len, groups.values())).items())
    # Request-level prediction is the implementable saving before translation/worker admission.
    old = {r["request"] for r in records}
    new = {r["request"][:48] + normalize(r["request"][48:]) for r in records}

    # Identical source/linkage required: guest stage hashes alone are insufficient.
    def identity(r, normalized=False, dynamic=False):
        return (
            r["sources"],
            r["root"],
            r["runtime"],
            normalize(r["state"]) if normalized else r["state"],
            b"" if dynamic else r["topology"],
        )

    # Actual native sharing retains guest hashes and every recipe byte except IA
    # topology; stream-output and advanced depth/stencil use the old build path.
    def native(r, normalized=False, dynamic=False):
        eligible = r["feedback"] == bytes(4) and not r["state"][21] and not (r["state"][9] & 2)
        return (
            r["stage_recipe"],
            r["root"],
            normalize(r["state"]) if normalized else r["state"],
            b"" if dynamic and eligible else r["topology"],
            r["feedback"],
        )

    native_raw = len({native(r) for r in records})
    native_a = len({native(r, True) for r in records})
    native_ab = len({native(r, True, True) for r in records})
    raw = len({identity(r) for r in records})
    a = len({identity(r, True) for r in records})
    ab = len({identity(r, True, True) for r in records})
    print(
        f"{folder.name}: graphics={len(records)} shader_sets={len(groups)} "
        f"PSOs/set={distribution} request_a_avoided={len(old) - len(new)} "
        f"exact_recipe_unique={raw} recipe_a_avoided={raw - a} "
        f"recipe_b_topology_upper_bound={a - ab} "
        f"native_unique={native_raw} native_a_avoided={native_raw - native_a} "
        f"native_b_avoided={native_a - native_ab} native_final={native_ab}"
    )
    if verbose:
        differences = collections.Counter()
        classes = collections.defaultdict(collections.Counter)
        for group in groups.values():
            for name, off, size in FIELDS:
                if len({r["state"][off : off + size] for r in group}) > 1:
                    differences[name] += 1
                    for r in group:
                        changed = r["state"][off : off + size] != r["normalized"][off : off + size]
                        classes[name]["a" if changed else "c"] += 1
            for name in ("topology", "root", "sources", "runtime", "feedback", "request"):
                if len({r[name] for r in group}) > 1:
                    differences[name] += 1
        for name, groups_count in sorted(differences.items()):
            classification = (
                "b (IA topology; shader specialization may require it)"
                if name == "topology"
                else ("a/c conditional" if classes[name]["a"] else "c")
            )
            print(f"  {name}: differing_shader_sets={groups_count} class={classification}")
        print(
            "  dynamic blend factor/stencil ref/depth bounds values/viewports/scissors: absent from key; b=0"
        )
        print(
            "  runtime: binding metadata + stream-output; sources: exact HLSL + descriptor counts; "
            "request: includes shader specialization (retained)"
        )
    return len(records), len(groups), len(old) - len(new), raw - a, a - ab


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folders", nargs="+", type=pathlib.Path)
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args()
    for folder in args.folders:
        census(folder, not args.summary)


if __name__ == "__main__":
    main()
