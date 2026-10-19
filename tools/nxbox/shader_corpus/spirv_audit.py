# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit optimized dataflow without erasing types, control flow, memory order or precision."""

import hashlib
import re
from collections import Counter

COMMUTATIVE = {
    "OpFAdd",
    "OpFMul",
    "OpIAdd",
    "OpIMul",
    "OpBitwiseAnd",
    "OpBitwiseOr",
    "OpBitwiseXor",
    "OpLogicalAnd",
    "OpLogicalOr",
    "OpLogicalEqual",
    "OpLogicalNotEqual",
    "OpIEqual",
    "OpINotEqual",
    "OpFOrdEqual",
    "OpFOrdNotEqual",
    "OpFUnordEqual",
    "OpFUnordNotEqual",
}


def graph(
    text, commute=False, indices=False, details=None, phi_anchors=None, phi_observations=None
):
    definitions = {}
    annotations = {}
    labels = {}
    phis = {}
    locations = {}
    body = []
    metadata = []
    block = None
    inside = False
    epoch = 0
    local_variables = {}
    for line in text.splitlines():
        if line.startswith(("OpDecorate ", "OpMemberDecorate ")):
            match = re.match(r"(\w+) (%\d+) (.*)", line)
            annotations.setdefault(match[2], []).append(match[1] + " " + match[3])
        match = re.match(r"(%\d+) = (.*)", line)
        if match:
            key, expression = match.groups()
            definitions[key] = expression
            if expression.startswith("OpFunction "):
                inside = True
            if expression == "OpLabel":
                labels[key] = f"block{len(labels)}"
                block = labels[key]
                epoch = 0
            if inside:
                locations[key] = (block, epoch)
                body.append((key, expression, block, epoch))
                if expression.startswith("OpPhi "):
                    phis[key] = (
                        f"phi:{block}:{len([p for p in phis.values() if p.startswith('phi:' + str(block) + ':')])}"
                    )
                if expression.startswith("OpVariable "):
                    local_variables[key] = f"variable{len(local_variables)}"
        elif inside:
            body.append((None, line, block, epoch))
        elif not line.startswith(("OpDecorate ", "OpMemberDecorate ")):
            metadata.append(line)
        if inside and re.search(
            r"\bOp(?:Store|CopyMemory|Atomic|ImageWrite|ControlBarrier|MemoryBarrier)", line
        ):
            epoch += 1
    if phi_anchors is not None:
        phis.update(phi_anchors)
    memo = {}
    visiting = set()

    def digest(value):
        fingerprint = hashlib.sha256(repr(value).encode()).hexdigest()
        if details is not None:
            details[fingerprint] = value
        return fingerprint

    def resolve(key):
        if key in labels:
            return labels[key]
        if key in phis:
            return phis[key]
        if key in memo:
            return memo[key]
        if key in visiting:
            raise ValueError(f"Unmodelled SSA cycle at {key}")
        visiting.add(key)
        expression = definitions[key]
        tokens = expression.split()
        opcode = tokens[0]
        operands = tokens[1:]
        converted = []
        for index, token in enumerate(operands):
            if token.startswith("%"):
                value = resolve(token)
                # Only positive 32-bit access-chain indices can ignore signedness.
                # No arithmetic operands or non-index constants are normalized.
                if indices and opcode in {"OpAccessChain", "OpInBoundsAccessChain"} and index >= 2:
                    const = definitions[token].split()
                    if len(const) == 3 and const[0] == "OpConstant" and const[2].isdigit():
                        ty = definitions[const[1]].split()
                        if ty[:2] == ["OpTypeInt", "32"] and int(const[2]) < 2**31:
                            value = f"index:{const[2]}"
                converted.append(value)
            else:
                converted.append(token)
        if commute and opcode in COMMUTATIVE:
            converted[1:3] = sorted(converted[1:3])
        if commute and opcode == "OpExtInst" and len(converted) == 6 and converted[2] == "Fma":
            converted[3:5] = sorted(converted[3:5])
        extra = sorted(annotations.get(key, []))
        if opcode == "OpVariable":
            extra += [local_variables.get(key, "global")]
        if opcode == "OpLoad":
            # Mutable loads stay tied to the preceding side effects. Immutable input
            # and cbuffer reads can move across unrelated output stores in one block.
            pointer = operands[1]
            root = pointer
            while definitions[root].startswith(("OpAccessChain ", "OpInBoundsAccessChain ")):
                root = definitions[root].split()[2]
            root_def = definitions[root].split()
            mutable = root_def[0] != "OpVariable" or root_def[-1] not in {
                "Input",
                "Uniform",
                "UniformConstant",
            }
            if root_def[0] == "OpVariable" and root_def[-1] == "Uniform":
                mutable = any("BufferBlock" in x for values in annotations.values() for x in values)
            if mutable:
                extra.append(str(locations[key]))
        memo[key] = digest((opcode, converted, extra))
        visiting.remove(key)
        return memo[key]

    values = Counter()
    effects = [
        ("metadata", re.sub(r"%\d+", lambda m: resolve(m[0]), line)) for line in sorted(metadata)
    ]
    for key, expression, block, epoch in body:
        if key is not None:
            if key in phis:
                # The phi result is a cycle anchor; separately compare all its edges.
                value = re.sub(r"%\d+", lambda m: resolve(m[0]), expression)
                if phi_observations is not None:
                    phi_observations[key] = digest((value, annotations.get(key, [])))
                values[digest((phis[key], value, annotations.get(key, [])))] += 1
            else:
                values[resolve(key)] += 1
        else:
            value = re.sub(r"%\d+", lambda m: resolve(m[0]), expression)
            effects.append((block, value))
    return values, effects


def phi_normalized_graph(text, commute=False, indices=False, details=None):
    """Refine phi names from incoming value graphs instead of declaration order.

    The anchor starts at its control-flow block; each round distinguishes phis by
    all incoming expressions and predecessor labels. This permits reordering phi
    declarations while continuing to compare every edge and every downstream use.
    """
    blocks = {}
    block = -1
    for line in text.splitlines():
        if re.match(r"%\d+ = OpLabel$", line):
            block += 1
        match = re.match(r"(%\d+) = OpPhi ", line)
        if match:
            blocks[match[1]] = block
    anchors = {key: f"phi:block{block}" for key, block in blocks.items()}
    # Use the same number of rounds on both modules, including cyclic phi graphs.
    for _ in range(8):
        observations = {}
        graph(
            text,
            commute=commute,
            indices=indices,
            phi_anchors=anchors,
            phi_observations=observations,
        )
        by_block = {}
        for key, fingerprint in observations.items():
            by_block.setdefault(blocks[key], set()).add(fingerprint)
        ranks = {
            block: {value: index for index, value in enumerate(sorted(values))}
            for block, values in by_block.items()
        }
        anchors = {
            key: f"phi:block{blocks[key]}:{ranks[blocks[key]][value]}"
            for key, value in observations.items()
        }
    return graph(text, commute=commute, indices=indices, phi_anchors=anchors, details=details)
