"""Generate a conservative Maxwell translation audit and reject coverage regressions."""

import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
FRONTEND = ROOT / "src/shader_recompiler/frontend/maxwell"
IMPL = FRONTEND / "translate/impl"
DOCUMENT = ROOT / "docs/maxwell-isa-coverage.md"
INITIAL_REVISION = "46cb09bd6780af7051cb550c76aca6fb710764e9"
FLOW = {
    "BRA",
    "JMP",
    "RET",
    "BRK",
    "CONT",
    "LONGJMP",
    "SYNC",
    "KIL",
    "PBK",
    "PCNT",
    "PEXIT",
    "PLONGJMP",
    "SSY",
    "BRX",
    "JMX",
    "EXIT",
    "PRET",
    "CAL",
    "JCAL",
}


BASELINE_GROUPS = {
    "full": [
        "BFE_reg",
        "BFI_reg",
        "DEPBAR",
        "HADD2_reg",
        "HFMA2_reg",
        "HMUL2_reg",
        "ISBERD",
        "ISCADD_reg",
        "MEMBAR",
        "NOP",
        "OUT_reg",
        "P2R_imm",
        "POPC_reg",
        "R2P_reg",
        "SEL_reg",
        "BFE_imm",
        "BFI_imm",
        "ISCADD_imm",
        "OUT_imm",
        "POPC_imm",
        "R2P_imm",
        "SEL_imm",
        "HADD2_imm",
        "HMUL2_imm",
        "HADD2_32I",
        "HFMA2_32I",
        "HMUL2_32I",
        "HFMA2_imm",
        "ISCADD32I",
    ],
    "not implemented": [
        "CCTLT",
        "B2R",
        "CS2R",
        "FCHK_cbuf",
        "FCHK_reg",
        "IDP_imm",
        "IDP_reg",
        "IMUL_cbuf",
        "IMUL_reg",
        "LEPC",
        "P2R_cbuf",
        "P2R_reg",
        "R2B",
        "STP",
        "TXA",
        "ATOM_cas",
        "BPT",
        "F2I_imm",
        "FCHK_imm",
        "GETCRSPTR",
        "GETLMEMBASE",
        "IDE",
        "IMUL_imm",
        "PRET",
        "PRMT_cr",
        "PRMT_rc",
        "PRMT_reg",
        "RRO_imm",
        "RTT",
        "SETCRSPTR",
        "SETLMEMBASE",
        "CCTL",
        "CCTLL",
        "PRMT_imm",
        "IMADSP_cr",
        "IMADSP_rc",
        "IMADSP_reg",
        "IMAD_cr",
        "IMAD_rc",
        "IMAD_reg",
        "SUATOM_cas",
        "VABSDIFF4",
        "ATOMS_cas",
        "IMADSP_imm",
        "IMAD_imm",
        "IMUL32I",
        "VABSDIFF",
        "VSHL",
        "VSHR",
        "VSET",
        "IMAD32I",
        "VADD",
        "LD",
        "ST",
    ],
    "partial": [
        "AL2P",
        "ALD",
        "AST",
        "BAR",
        "BFE_cbuf",
        "BFI_cr",
        "BFI_rc",
        "CSET",
        "CSETP",
        "DADD_cbuf",
        "DADD_reg",
        "DMNMX_cbuf",
        "DMNMX_reg",
        "DMUL_cbuf",
        "DMUL_reg",
        "F2F_cbuf",
        "F2F_reg",
        "F2I_cbuf",
        "F2I_reg",
        "FADD_cbuf",
        "FADD_reg",
        "FLO_cbuf",
        "FLO_reg",
        "FMNMX_cbuf",
        "FMNMX_reg",
        "FMUL_cbuf",
        "FMUL_reg",
        "FSWZADD",
        "HSET2_reg",
        "HSETP2_reg",
        "I2F_cbuf",
        "I2F_reg",
        "I2I_cbuf",
        "I2I_reg",
        "IADD_cbuf",
        "IADD_reg",
        "IMNMX_cbuf",
        "IMNMX_reg",
        "ISCADD_cbuf",
        "LDC",
        "LDG",
        "LDL",
        "LDS",
        "LEA_hi_reg",
        "LEA_lo_reg",
        "LOP3_reg",
        "LOP_cbuf",
        "LOP_reg",
        "MOV_cbuf",
        "MOV_reg",
        "MUFU",
        "OUT_cbuf",
        "PIXLD",
        "POPC_cbuf",
        "PSET",
        "PSETP",
        "R2P_cbuf",
        "RED",
        "RRO_cbuf",
        "RRO_reg",
        "S2R",
        "SEL_cbuf",
        "SHFL",
        "SHF_l_reg",
        "SHF_r_reg",
        "SHL_cbuf",
        "SHL_reg",
        "SHR_cbuf",
        "SHR_reg",
        "STG",
        "STL",
        "STS",
        "SYNC",
        "TMML",
        "TMML_b",
        "TXQ",
        "TXQ_b",
        "VOTE",
        "VOTE_vtg",
        "VSETP",
        "BRA",
        "BRK",
        "BRX",
        "CAL",
        "CONT",
        "DADD_imm",
        "DFMA_cr",
        "DFMA_rc",
        "DFMA_reg",
        "DMNMX_imm",
        "DMUL_imm",
        "DSETP_cbuf",
        "DSETP_reg",
        "EXIT",
        "F2F_imm",
        "FADD_imm",
        "FCMP_cr",
        "FCMP_rc",
        "FCMP_reg",
        "FLO_imm",
        "FMNMX_imm",
        "FMUL_imm",
        "FSETP_cbuf",
        "FSETP_reg",
        "I2F_imm",
        "I2I_imm",
        "IADD3_cbuf",
        "IADD3_reg",
        "IADD_imm",
        "ICMP_cr",
        "ICMP_rc",
        "ICMP_reg",
        "IMNMX_imm",
        "ISETP_cbuf",
        "ISETP_reg",
        "ISET_cbuf",
        "ISET_reg",
        "JCAL",
        "JMP",
        "JMX",
        "KIL",
        "LEA_lo_cbuf",
        "LEA_lo_imm",
        "LONGJMP",
        "LOP_imm",
        "MOV32I",
        "MOV_imm",
        "PBK",
        "PCNT",
        "PEXIT",
        "PLONGJMP",
        "RAM",
        "RET",
        "SAM",
        "SHF_l_imm",
        "SHF_r_imm",
        "SHL_imm",
        "SHR_imm",
        "SSY",
        "DFMA_imm",
        "DSETP_imm",
        "FCMP_imm",
        "FSETP_imm",
        "IADD3_imm",
        "ICMP_imm",
        "ISETP_imm",
        "ISET_imm",
        "SULD",
        "SURED",
        "SUST",
        "TEX_b",
        "TLD4_b",
        "TXD",
        "TXD_b",
        "XMAD_reg",
        "DSET_cbuf",
        "DSET_reg",
        "FFMA_cr",
        "FFMA_rc",
        "FFMA_reg",
        "SUATOM",
        "TLD4S",
        "XMAD_imm",
        "XMAD_rc",
        "ATOM",
        "ATOMS",
        "DSET_imm",
        "FFMA_imm",
        "FMUL32I",
        "FSET_cbuf",
        "FSET_reg",
        "HADD2_cbuf",
        "HMUL2_cbuf",
        "HSET2_cbuf",
        "HSET2_imm",
        "HSETP2_cbuf",
        "HSETP2_imm",
        "IPA",
        "TLD",
        "TLD_b",
        "VMAD",
        "FSET_imm",
        "IADD32I",
        "LOP3_cbuf",
        "VMNMX",
        "XMAD_cr",
        "FADD32I",
        "FFMA32I",
        "HFMA2_cr",
        "HFMA2_rc",
        "LEA_hi_cbuf",
        "LOP32I",
        "LOP3_imm",
        "TEXS",
        "TLD4",
        "TLDS",
        "TEX",
    ],
}


def baseline():
    return {
        "counts": {status: len(names) for status, names in BASELINE_GROUPS.items()},
        "visitor_stubs": 60,
        "throw_sites": 206,
        "rows": [
            {"instruction": name, "status": status}
            for status, names in BASELINE_GROUPS.items()
            for name in names
        ],
    }


def source(path, revision=None):
    if revision:
        return subprocess.check_output(
            ["git", "show", f"{revision}:{path.relative_to(ROOT)}"], cwd=ROOT, text=True
        )
    return path.read_text()


def mask_literals(text):
    return re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match[0]),
        text,
        flags=re.DOTALL,
    )


def functions(text):
    masked = mask_literals(text)
    definitions = []
    pattern = r"\b(?:void|bool|u32|u64|IR::\w+|Shader::\w+|std::\w+(?:<[^;{}]+>)?)\s+(\w+(?:::\w+)*)\s*\([^;{}]*?\)\s*\{"
    for match in re.finditer(pattern, masked):
        depth, end = 1, match.end()
        while depth:
            depth += (masked[end] == "{") - (masked[end] == "}")
            end += 1
        definitions.append((match[1], text[match.end() : end - 1]))
    return definitions


def audit(revision=None):
    files = {path: source(path, revision) for path in sorted(IMPL.glob("*.cpp"))}
    definitions = {path: functions(text) for path, text in files.items()}
    opcodes = set(
        re.findall(r"^INST\((\w+)", source(FRONTEND / "maxwell.inc", revision), re.MULTILINE)
    )
    visitors = {}
    for path, items in definitions.items():
        for name, body in items:
            if name.startswith("TranslatorVisitor::") and name.split("::")[-1] in opcodes:
                opcode = name.split("::")[-1]
                if opcode in visitors:
                    raise AssertionError(f"Duplicate visitor {opcode}")
                visitors[opcode] = (path, body)
    common_paths = [IMPL / "impl.cpp", IMPL / "common_funcs.cpp"]
    flow_text = source(FRONTEND / "control_flow.cpp", revision)
    all_text = "\n".join(files.values())
    rows = []
    for match in re.finditer(
        r'^INST\((\w+),\s*"([^"]+)",\s*"([^"]+)"\)',
        source(FRONTEND / "maxwell.inc", revision),
        re.MULTILINE,
    ):
        opcode, display, encoding = match.groups()
        if opcode not in visitors:
            raise AssertionError(f"Missing visitor {opcode}")
        path, root = visitors[opcode]
        reached, pending = [], [(path, opcode, root)]
        seen = set()
        while pending:
            owner, name, body = pending.pop()
            key = (owner, name, body)
            if key in seen:
                continue
            seen.add(key)
            reached.append(body)
            calls = set(re.findall(r"\b(\w+)\s*\(", mask_literals(body)))
            for candidate in dict.fromkeys([owner, *common_paths]):
                for helper, helper_body in definitions[candidate]:
                    if helper.split("::")[-1] in calls:
                        pending.append((candidate, helper, helper_body))
        body = "\n".join(reached)
        restrictions = set(re.findall(r'throw\s+NotImplementedException\(\s*"([^"]+)"', body))
        reserved = {item for item in restrictions if item.startswith("Invalid PRMT mode")}
        restrictions -= reserved
        stubs = sorted(set(re.findall(r'"([^"\n]*STUBBED[^"\n]*)"', body)))
        if opcode in FLOW:
            # CFG consumes these before ordinary visitor dispatch. Conservatively
            # include all CFG restrictions, including indirect target discovery.
            restrictions.update(
                re.findall(r'throw\s+NotImplementedException\(\s*"([^"]+)"', flow_text)
            )
            stubs = []
        if opcode.startswith("RRO_") and "ThrowNotImplemented" not in root:
            stubs.append("Range reduction currently passes through abs/neg only")
        if opcode == "PRET":
            status = "not implemented"
        elif opcode not in FLOW and (
            "ThrowNotImplemented(" in root or re.match(r"\s*throw NotImplementedException", root)
        ):
            status = "not implemented"
        elif restrictions or stubs:
            status = "partial"
        else:
            status = "full"
        fields = sorted(
            set(re.findall(r"BitField<\s*(\d+),\s*(\d+),\s*[^>]+>\s+(\w+)", body)),
            key=lambda field: (int(field[0]), int(field[1]), field[2]),
        )
        modifiers = ", ".join(f"{name}[{offset}:{width}]" for offset, width, name in fields)
        details = sorted(restrictions) + stubs
        if opcode in FLOW:
            details.insert(0, "CFG lowering; visitor stub is not the executable path")
        if reserved:
            details.append("Reserved PRMT modes 7–15 rejected")
        rows.append(
            {
                "instruction": opcode,
                "display": display,
                "encoding": encoding,
                "status": status,
                "fields": modifiers or "See source operands",
                "restrictions": details,
                "file": str(path.relative_to(ROOT)),
            }
        )
    counts = Counter(row["status"] for row in rows)
    return {
        "rows": rows,
        "counts": dict(sorted(counts.items())),
        "total": len(rows),
        "throw_sites": all_text.count("throw NotImplementedException"),
        "visitor_stubs": sum("ThrowNotImplemented(" in body for _, body in visitors.values()),
    }


def render(result, before):
    counts = result["counts"]
    lines = [
        "# Maxwell ISA translation coverage",
        "",
        "Generated by `python3 tests/port/test_t23_maxwell_coverage.py --write`. Do not edit the table by hand.",
        "",
        "Scope: the 280 encoding entries in `frontend/maxwell/maxwell.inc` (GM20B / SM5.x), not every NVIDIA architecture. This is a conservative source audit, not a claim of silicon equivalence. `full` means a visitor and its reachable local/shared translation helpers have no known rejection or logged stub. `partial` lists their rejection messages and stub notes. Bounds/alignment checks are retained. CFG instructions are counted as partial because target discovery and condition handling remain restricted, even when their ordinary visitor throws. Reserved PRMT modes are invalid encodings, not missing valid modifiers. Helper overloads are conservatively combined; a restriction can apply to only one operand form.",
        "",
        "Each field is `name[least-significant-bit:width]`; the encoding column is the decoder's high 16 bits. Shared IR, optimization and backend restrictions can additionally reject a translated instruction. This table does not prove flags, rounding, memory ordering, texture results or control flow correct; independent execution tests and console measurements are still required.",
        "",
        f"Baseline revision: `{INITIAL_REVISION}` (the worktree started here; requested base `ea3244c9c` is an ancestor).",
        "",
        "| Measure | Before | After |",
        "| --- | ---: | ---: |",
        *[
            f"| {status} | {before['counts'].get(status, 0)} | {counts.get(status, 0)} |"
            for status in ("full", "partial", "not implemented")
        ],
        f"| Visitor stubs | {before['visitor_stubs']} | {result['visitor_stubs']} |",
        f"| Explicit rejection sites in translation files | {before['throw_sites']} | {result['throw_sites']} |",
        "",
        "References: [envytools GM107 decoder](https://github.com/envytools/envytools/blob/master/envydis/gm107.c), [Maxwell ISA notes](https://envytools.readthedocs.io/en/latest/hw/graph/maxwell/cuda/isa.html), [NVIDIA PTX byte permutation](https://docs.nvidia.com/cuda/archive/11.5.0/parallel-thread-execution/index.html#data-movement-and-conversion-instructions-prmt), [PTX integer multiplication](https://docs.nvidia.com/cuda/archive/11.5.0/parallel-thread-execution/index.html#integer-arithmetic-instructions-mul). PTX defines arithmetic behavior; GM107 supplies SASS encoding. Unsupported semantics are preserved as explicit failures rather than substituted results.",
        "",
        "T23 implements PRMT generic/sign replication plus F4E/B4E/RC8/ECL/ECR/RC16 across reg/cr/rc/imm, and IMUL/IMAD/IMAD32I low/high products including independently signed operands. High multiplication uses 16-bit limbs and existing U32 IR. IMUL.CC, IMAD.CC/X/SAT and IMAD32I.CC/NEG/PO remain unsupported. SASS PRMT operands are source A, selector, source B; tests include a fixed immediate-selector packing case. P2R reg/cbuf now share the immediate form's masked predicate/condition-code packing, including all four byte destinations. No HLSL emitter changes are required. The native differential test evaluates actual emitted IR against byte-array and native 64-bit reference arithmetic, including all 65,536 generic selectors, random inputs, sign boundaries, register aliasing, and every 7-bit predicate mask/state and CC byte selection (651,265 comparisons).",
        "",
        "Remaining work includes every row marked not implemented, valid rejected modifiers in partial rows, silent legacy stubs, precise RRO reduction, system register defaults, and host/backend execution equivalence. Cache operations, barriers, texture/surface variants and CAS need representation and ordering work; flow extensions need CFG/structured lowering, not visitor no-ops. Console tests must verify PRMT SASS operand order/modes and signed IMUL/IMAD.HI behavior, masked P2R predicate/CC packing into each byte, then shader output comparisons for unaffected corpus stages. DXC success measures compilation, not pixels.",
        "",
        "| Instruction | High-bit encoding | Status | Operand/modifier fields | Rejections / limitations | Source |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for row in result["rows"]:
        details = "; ".join(row["restrictions"]) or "No known translation rejection"
        details = details.replace("|", "\\|").replace("\n", " ")
        lines.append(
            f"| `{row['instruction']}` | `{row['encoding']}` | {row['status']} | {row['fields']} | {details} | [{Path(row['file']).name}](../{row['file']}) |"
        )
    return "\n".join(lines) + "\n"


class MaxwellCoverageTests(unittest.TestCase):
    def test_all_encodings_have_visitors(self):
        result = audit()
        self.assertEqual(result["total"], 280)
        self.assertEqual(len({row["instruction"] for row in result["rows"]}), 280)

    def test_document_matches_live_source(self):
        before = baseline()
        self.assertEqual(DOCUMENT.read_text(), render(audit(), before))

    def test_coverage_does_not_drop(self):
        before, after = baseline(), audit()
        rank = {"not implemented": 0, "partial": 1, "full": 2}
        old_rows = {row["instruction"]: row for row in before["rows"]}
        for row in after["rows"]:
            self.assertGreaterEqual(
                rank[row["status"]],
                rank[old_rows[row["instruction"]]["status"]],
                row["instruction"],
            )
        self.assertLessEqual(after["visitor_stubs"], before["visitor_stubs"] - 13)
        self.assertLessEqual(after["counts"]["not implemented"], 39)
        self.assertGreaterEqual(after["counts"]["full"], 32)
        statuses = {row["instruction"]: row["status"] for row in after["rows"]}
        for name in ("PRMT_reg", "PRMT_imm", "P2R_reg"):
            self.assertEqual(statuses[name], "full", name)
        for name in (
            "IMUL_reg",
            "IMUL_cbuf",
            "IMUL_imm",
            "IMUL32I",
            "IMAD_reg",
            "IMAD_cr",
            "IMAD_rc",
            "IMAD_imm",
            "IMAD32I",
            "P2R_cbuf",
        ):
            self.assertNotEqual(statuses[name], "not implemented", name)
        for row in after["rows"]:
            if row["instruction"].startswith("PRMT_"):
                # Constant buffer bounds are separately audited by shared helpers.
                self.assertFalse(any("Instruction" in item for item in row["restrictions"]))
                self.assertIn("Reserved PRMT modes 7–15 rejected", row["restrictions"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--report", action="store_true")
    args, rest = parser.parse_known_args()
    if args.write or args.report:
        result = audit()
        if args.write:
            DOCUMENT.write_text(render(result, baseline()))
        print({key: value for key, value in result.items() if key != "rows"})
    else:
        unittest.main(argv=[__file__, *rest])
