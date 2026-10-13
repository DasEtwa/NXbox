"""Audit every decoder row; source coverage is not semantic ISA conformance."""

import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
FRONTEND = Path("src/dynarmic/src/dynarmic/frontend")
BASE = "ea3244c9cb"
ENTRY = re.compile(r'^INST\((\w+),\s*"([^"]*)"\s*,\s*"([^"]+)"\)', re.M)
FUNCTION = re.compile(r"bool TranslatorVisitor::(\w+)\([^)]*\)\s*\{")
TABLES = {"A64": ["a64"], "A32": ["arm", "asimd", "vfp", "thumb16", "thumb32"]}


def read_source(path, revision=None):
    if revision:
        return subprocess.check_output(
            ["git", "show", f"{revision}:{path.as_posix()}"], cwd=ROOT, text=True
        )
    return (ROOT / path).read_text()


def inventory(revision=None):
    rows = []
    for arch, tables in TABLES.items():
        bodies = {}
        helpers = {}
        for absolute in sorted((ROOT / FRONTEND / arch / "translate").rglob("*.cpp")):
            path = absolute.relative_to(ROOT)
            source = read_source(path, revision)
            local_helpers = {}
            helper_pattern = re.compile(r"bool (\w+)\([^)]*\)\s*\{")
            for match in helper_pattern.finditer(source):
                end, depth = match.end(), 1
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                local_helpers[match[1]] = source[match.end() : end - 1]
            helpers[path] = local_helpers
            for match in FUNCTION.finditer(source):
                end, depth = match.end(), 1
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                body = source[match.end() : end - 1]
                bodies[match[1]] = (path, body)
        for table in tables:
            source = read_source(FRONTEND / arch / "decoder" / f"{table}.inc", revision)
            for index, (handler, name, bits) in enumerate(ENTRY.findall(source), 1):
                path, body = bodies.get(handler, (None, ""))
                for helper, helper_body in helpers.get(path, {}).items():
                    if re.search(rf"\b{helper}\(", body):
                        body += "\n" + helper_body
                code = re.sub(r"//[^\n]*|/\*.*?\*/", "", body, flags=re.S).strip()
                if path is None:
                    status = "missing"
                elif re.search(r"InterpretThisInstruction|NOT_IMPLEMENTED|UNIMPLEMENTED", code):
                    status = "fallback/marker"
                elif re.fullmatch(r"UNREACHABLE\(\);", code):
                    status = "unreachable stub"
                elif re.fullmatch(
                    r"return (?:UndefinedInstruction|UnallocatedEncoding)\(\);", code
                ) or re.fullmatch(
                    r"return RaiseException\(Exception::UnallocatedEncoding\);", code
                ):
                    status = (
                        "architectural undefined"
                        if handler.endswith("UDF") or handler == "UnallocatedEncoding"
                        else "undefined stub"
                    )
                else:
                    status = "translated"
                guards = sorted(
                    set(
                        re.findall(
                            r"\b(UndefinedInstruction|UnpredictableInstruction|UnallocatedEncoding|ReservedValue|DecodeError|RaiseException|UNREACHABLE|ASSERT|ASSERT_MSG)\(",
                            code,
                        )
                    )
                )
                if re.search(
                    r"if\s*\([^)]*\*datasize\s*==\s*16[^)]*\)\s*\{\s*return\s+(?:\w+\.)?UnallocatedEncoding\(\);",
                    code,
                ) or re.search(r"ASSERT(?:_MSG)?\([^;]*esize\s*!=\s*16", code):
                    guards.append("FP16 unsupported")
                rows.append(
                    {
                        "arch": arch,
                        "table": table,
                        "index": index,
                        "handler": handler,
                        "name": name,
                        "bits": bits,
                        "status": status,
                        "guards": guards,
                        "source": str(path) if path else "",
                    }
                )
    return rows


class CpuConformanceTests(unittest.TestCase):
    def test_all_decoder_entries_have_handlers_and_valid_width(self):
        rows = inventory()
        self.assertEqual(
            Counter(row["table"] for row in rows),
            {"a64": 643, "arm": 261, "asimd": 147, "vfp": 62, "thumb16": 79, "thumb32": 239},
        )
        for row in rows:
            with self.subTest(table=row["table"], index=row["index"]):
                self.assertNotEqual(row["status"], "missing")
                self.assertEqual(len(row["bits"]), 16 if row["table"] == "thumb16" else 32)

    def test_unprivileged_transfers_are_decoded_and_translated(self):
        rows = [
            row
            for row in inventory()
            if re.match(r"arm_(?:LDR|STR)(?:SB|SH|B|H)?T_", row["handler"])
        ]
        self.assertEqual(len(rows), 17)
        self.assertEqual(len({row["handler"] for row in rows}), 17)
        for row in rows:
            self.assertEqual(row["status"], "translated")
            self.assertNotIn("UndefinedInstruction", row["guards"])
        source = (ROOT / FRONTEND / "A32/translate/impl/load_store.cpp").read_text()
        self.assertIn("IR::AccType::UNPRIV", source)
        fuzz = (ROOT / "src/dynarmic/tests/A32/fuzz_arm.cpp").read_text()
        self.assertIn("Unprivileged transfers differential", fuzz)
        self.assertIn("std::mt19937 random{0x1932}", fuzz)

    def test_remaining_stubs_are_explicitly_accounted_for(self):
        rows = inventory()
        stubs = [
            row
            for row in rows
            if row["status"] in ("undefined stub", "unreachable stub", "fallback/marker")
        ]
        self.assertEqual(
            {row["handler"] for row in stubs},
            {"arm_RFE", "arm_SRS", "arm_LDM_usr", "arm_LDM_eret", "arm_STM_usr"},
        )
        self.assertEqual(len(stubs), 5)


def report(destination):
    before, after = inventory(BASE), inventory()
    lines = [
        "# CPU conformance audit",
        "",
        f"Base: `{BASE}`. Track: `team/t19-cpu-conformance`.",
        "",
        "## What is measured",
        "",
        "Every ordered `INST` row in the six decoder tables is enumerated below. Aliases and reserved encodings count as rows, not distinct instructions. `translated` means a C++ visitor body exists and is not an unconditional stub; it does **not** prove architectural correctness. Guard flags include expected checks for invalid operands and architectural exceptions. This static audit does not inspect backend lowering or recursively prove helper semantics.",
        "",
        "## Before / after",
        "",
        "| Table | Rows | Translated before | Translated after | Architectural undefined | Remaining stubs |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for table in ["a64", "arm", "asimd", "vfp", "thumb16", "thumb32"]:
        old = Counter(row["status"] for row in before if row["table"] == table)
        new = Counter(row["status"] for row in after if row["table"] == table)
        count = sum(new.values())
        lines.append(
            f"| {table} | {count} | {old['translated']} | {new['translated']} | {new['architectural undefined']} | {new['undefined stub'] + new['unreachable stub']} |"
        )
    lines += [
        "",
        "17 decoder rows (8 instruction families, 16 valid operand forms plus a PC-base guard) now dispatch A32 LDRBT/LDRHT/LDRSBT/LDRSHT/LDRT/STRBT/STRHT/STRT instead of unconditionally raising undefined. Valid forms perform a post-indexed UNPRIV access, preserve condition execution, extend loads correctly, and write back the base after the transfer. PC and base/destination overlap remain unpredictable. The prior general fuzz exclusion is removed.",
        "",
        "The five remaining unconditional unreachable handlers are privileged RFE/SRS and banked-register LDM/STM forms. They are outside the user-mode execution target; this audit does not claim their privileged semantics are implemented. Architectural UDF and reserved SIMD encodings intentionally raise exceptions. There are zero direct InterpretThisInstruction/NOT_IMPLEMENTED/UNIMPLEMENTED markers in decoder handlers.",
        "",
        "## Differential evidence",
        "",
        "The targeted Catch2 `[conformance]` test runs 4,096 deterministic randomized cases against the existing Unicorn A32 reference harness (16 forms × 256 states). It compares registers, CPSR, extended registers, FPSCR and written memory through RunTestInstance. Cases cover add/subtract, all executable condition codes, random NZCV/carry, all register shift kinds including zero shifts/RRX, sign extension and post-indexed writeback. Existing full A64/A32/Thumb randomized suites also run in CI with Unicorn enabled. A passing source audit cannot replace these executable tests.",
        "",
        "## Scope limits and priorities",
        "",
        "No real-game instruction histogram was available in this worktree. Priority therefore follows ISA categories: valid user-mode memory instructions first. LSE (CAS/CASP, SWP, LDADD/LDCLR/LDEOR/LDSET and min/max families) has **no decoder rows** here and is not covered by the row denominator; adding concurrency-correct backend atomics requires a separate measured change. Missing ISA extensions cannot be inferred from existing tables. The tables include AdvSIMD/FP, AES/SHA/CRC and later crypto/flag extensions, but this is not a claim of complete ARMv8 coverage. A full ISA inventory, backend differential sweep, FP exception/NaN audit and memory-ordering litmus tests remain necessary for the stated all-instruction goal.",
        "",
        "Rosetta can execute x86-64 on this Mac, but local heavy compilation was avoided after measuring swap at 16.7/17.4 GB and only 8.9 GB disk free. The dedicated workflow builds and executes the real dynarmic suite on Windows and Ubuntu x64, outside the graphics renderer work.",
        "",
        "## Console follow-up",
        "",
        "Run legal owner-provided 32-bit software exercising the unprivileged transfer forms on Xbox UWP; verify callback memory accesses and W^X JIT execution, then collect an anonymized instruction histogram (opcode counts only, no game/firmware bytes or keys). Also compare FP/crypto/CRC outputs with real Switch hardware and validate exclusive-monitor ordering under contention. Windows desktop Unicorn comparisons do not establish Xbox AppContainer behavior.",
        "",
        "Instruction behavior reference: [Arm Architecture Reference Manual](https://documentation-service.arm.com/static/67e40a9c4191d739c8e9bd2c). Reference interpreter: [Unicorn 2.1.4](https://github.com/unicorn-engine/unicorn/releases/tag/2.1.4).",
        "",
        "## Complete ordered decoder inventory",
        "",
        "`guards` lists direct exceptional/validation calls, including legal architectural traps. An empty guard column does not establish that every operand encoding is legal.",
        "",
    ]
    for table in ["a64", "arm", "asimd", "vfp", "thumb16", "thumb32"]:
        lines += [
            f"### {table}",
            "",
            "| Row | Handler | Name | Encoding | Status | Guards | Source |",
            "|---:|---|---|---|---|---|---|",
        ]
        for row in after:
            if row["table"] == table:
                lines.append(
                    f"| {row['index']} | `{row['handler']}` | {row['name']} | `{row['bits']}` | {row['status']} | {', '.join(row['guards'])} | `{Path(row['source']).name}` |"
                )
        lines.append("")
    output = "\n".join(lines)
    marker = "## Complete ordered decoder inventory"
    destination = Path(destination)
    if destination.exists():
        # Preserve the curated findings while refreshing the generated inventory.
        existing = destination.read_text()
        if marker in existing:
            output = existing.split(marker, 1)[0] + marker + output.split(marker, 1)[1]
    destination.write_text(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--report")
    args, remaining = parser.parse_known_args()
    if args.report:
        report(args.report)
    else:
        unittest.main(argv=[__file__, *remaining])
