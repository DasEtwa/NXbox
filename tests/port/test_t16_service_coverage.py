"""Inventory every registered HLE command; generate the auditable source coverage report.

Run directly with --generate to refresh docs/service-coverage.md. No network required.
"""

import argparse
import json
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICE_ROOT = ROOT / "src/core/hle/service"
REPORT = ROOT / "docs/service-coverage.md"
TABLE = re.compile(
    r"\b(?:static\s+)?(?:const(?:expr)?\s+)?FunctionInfo(?:Typed<[^>]+>)?\s+(\w+)\s*\[\s*\]\s*=\s*\{"
)
INLINE_TABLE = re.compile(r"\bRegisterHandlers(Tipc)?\s*\(\s*\{")
ENTRY = re.compile(
    r'\{\s*(0x[\da-fA-F]+|\d+)\s*,\s*(nullptr|(?:[DC]<)?&[^,{}\"]+)\s*,\s*"([^"]*)"\s*\}'
)
METHOD = re.compile(r"\b(?:void|Result)\s+([\w:]+)\s*\([^;{}]*\)\s*(?:const\s*)?\{")
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)


def mask_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: " " * len(m[0]), text, flags=re.S)


def block_end(text, start):
    depth = 0
    for token in TOKEN.finditer(text, start):
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if depth == 0:
                return token.start()
    raise ValueError(f"Unclosed block at {start}")


def class_at(text, position):
    candidates = []
    for match in re.finditer(r"\bclass\s+(\w+)\s*(?:final\s*)?(?::[^;{}]*)?\{", text):
        if match.start() < position < block_end(text, match.end() - 1):
            candidates.append(match[1])
    return candidates[-1] if candidates else None


def methods(text):
    clean = mask_comments(text)
    result = {}
    for match in METHOD.finditer(clean):
        name = match[1]
        if "::" not in name:
            owner = class_at(clean, match.start())
            if owner:
                name = f"{owner}::{name}"
        result[name] = text[match.end() : block_end(text, match.end() - 1)]
    return result


@dataclass(frozen=True)
class Command:
    source: str
    table: str
    interface: str
    protocol: str
    command: int
    name: str
    handler: str
    status: str
    comment: str
    references: tuple[str, ...]

    @property
    def identity(self):
        return (self.source, self.table, self.protocol, self.command)


def parse_tables(text, source, definitions):
    clean = mask_comments(text)
    records = []
    occurrences = Counter()
    references = tuple(sorted(set(re.findall(r"https?://[^\s<>]+", text))))
    tables = [(m, m[1], False, None) for m in TABLE.finditer(clean)]
    tables += [
        (m, "inline_tipc" if m[1] else "inline_cmif", True, "TIPC" if m[1] else "CMIF")
        for m in INLINE_TABLE.finditer(clean)
    ]
    for match, name, inline, protocol in sorted(tables, key=lambda t: t[0].start()):
        end = block_end(clean, match.end() - 1)
        occurrences[name] += 1
        table = f"{name}#{occurrences[name]}"
        body = clean[match.end() : end]
        entries = list(ENTRY.finditer(body))
        # Every initializer must be understood: do not silently omit a new table syntax.
        residue = ENTRY.sub("", body).replace(",", "").strip()
        if residue:
            raise ValueError(f"Unsupported table syntax: {source}:{table}: {residue[:100]}")
        if not inline:
            registration = re.search(rf"RegisterHandlers(Tipc)?\s*\(\s*{name}\b", clean[end:])
            if not registration:
                raise ValueError(f"No registration for {source}:{table}")
            protocol = "TIPC" if registration[1] else "CMIF"
        owner = class_at(clean, match.start())
        if not owner:
            constructors = list(re.finditer(r"\b(\w+)::\1\s*\(", clean[: match.start()]))
            owner = constructors[-1][1] if constructors else "unknown"
        for entry in entries:
            handler = entry[2].strip()
            target = (
                handler.removeprefix("D<").removeprefix("C<").removeprefix("&").removesuffix(">")
            )
            target = target.split("<", 1)[0]
            implementation = definitions.get((source, target), definitions.get(target))
            if implementation is None and handler != "nullptr":
                method_name = target.rsplit("::", 1)[-1]
                candidates = {
                    body
                    for key, body in definitions.items()
                    if isinstance(key, tuple)
                    and Path(key[0]).parent == Path(source).parent
                    and key[1].rsplit("::", 1)[-1] == method_name
                }
                local_candidates = {
                    body
                    for key, body in definitions.items()
                    if isinstance(key, tuple)
                    and key[0] == source
                    and key[1].rsplit("::", 1)[-1] == method_name
                }
                if len(local_candidates) == 1:
                    implementation = local_candidates.pop()
                elif len(candidates) == 1:
                    implementation = candidates.pop()
            if handler == "nullptr":
                status = "nullptr-unimplemented"
            else:
                if implementation is None:
                    raise ValueError(f"Handler body not resolved: {source}: {target}")
                marked = re.search(r"STUBBED|UNIMPLEMENTED", implementation, re.I)
                success = re.search(r"ResultSuccess|R_SUCCEED\s*\(", implementation)
                status = "stub-success" if marked and success else "implemented"
            absolute_end = match.end() + entry.end()
            line_end = text.find("\n", absolute_end)
            tail = text[absolute_end : line_end if line_end >= 0 else len(text)]
            comment = tail.split("//", 1)[1].strip() if "//" in tail else ""
            records.append(
                Command(
                    source,
                    table,
                    owner,
                    protocol,
                    int(entry[1], 0),
                    entry[3],
                    handler,
                    status,
                    comment,
                    references,
                )
            )
    return records


def inventory(root=SERVICE_ROOT, sources=None):
    sources = (
        sources
        if sources is not None
        else {
            p.relative_to(root).as_posix(): p.read_text()
            for p in sorted(root.rglob("*"))
            if p.suffix in (".cpp", ".h")
        }
    )
    definitions = {}
    bases = {}
    for source, text in sources.items():
        for name, body in methods(text).items():
            definitions[(source, name)] = body
            # Global names can be ambiguous; only use globally unique definitions.
        for match in re.finditer(
            r"\bclass\s+(\w+)\s*(?:final\s*)?:\s*(?:public\s+)?([\w:]+)", mask_comments(text)
        ):
            bases[(str(Path(source).parent), match[1])] = match[2]
    by_name = defaultdict(list)
    for (source, name), body in definitions.items():
        by_name[name].append(body)
    for name, bodies in by_name.items():
        if len(set(bodies)) == 1:
            definitions[name] = bodies[0]

    def resolve(directory, target, visited):
        if (directory, target) in visited:
            return None
        visited.add((directory, target))
        candidates = {
            body
            for key, body in definitions.items()
            if isinstance(key, tuple)
            and str(Path(key[0]).parent) == directory
            and (key[1] == target or key[1].endswith("::" + target))
        }
        if len(candidates) == 1:
            return candidates.pop()
        owner, method = target.rsplit("::", 1)
        base = bases.get((directory, owner))
        if base:
            # A namespace-qualified base resides in that service directory.
            parts = base.split("::")
            base_directory = parts[0].lower() if len(parts) > 1 else directory
            return resolve(base_directory, parts[-1] + "::" + method, visited)
        return None

    for source, text in sources.items():
        for entry in ENTRY.finditer(mask_comments(text)):
            target = (
                entry[2].removeprefix("D<").removeprefix("C<").removeprefix("&").removesuffix(">")
            )
            target = target.strip().split("<", 1)[0]
            if target != "nullptr" and (source, target) not in definitions:
                body = resolve(str(Path(source).parent), target, set())
                if body is not None:
                    definitions[(source, target)] = body
    return [
        command
        for source, text in sources.items()
        for command in parse_tables(text, source, definitions)
    ]


def owned(command):
    return not (command.source.startswith("ldn/") or command.source.startswith("am/applets/"))


def totals(commands):
    counts = Counter(command.status for command in commands)
    return ", ".join(
        f"{key}={counts[key]}" for key in ("implemented", "stub-success", "nullptr-unimplemented")
    )


def render(commands):
    groups = defaultdict(list)
    for command in commands:
        groups[(command.source, command.table, command.interface, command.protocol)].append(command)
    lines = [
        "# HLE service command coverage",
        "",
        "Generated by `python3 tests/port/test_t16_service_coverage.py --generate`.",
        "",
        f"All services: {totals(commands)}; total={len(commands)}.",
        f"T16-owned services: {totals([c for c in commands if owned(c)])}.",
        "",
        "## Method and limits",
        "",
        "The inventory includes inline RegisterHandlers initializer lists and every FunctionInfo/FunctionInfoTyped table in service .cpp/.h files, including CMIF D/C wrappers, TIPC, conditional editor tables, ldn and am/applets (read-only inventory). Source/table identifies independent registrations even when interface names are reused. Dynamic port names and firmware-gated registrations are not expanded into fictitious runtime instances.",
        "",
        "Implemented means a non-null handler with a resolved body and no explicit STUBBED/UNIMPLEMENTED marker returning success. Stub-success means a body has both such a marker and ResultSuccess/R_SUCCEED; this is a conservative source heuristic, not hardware conformance or proof that every path succeeds. Delegated helpers and inaccurate upstream markers still need review. nullptr-unimplemented means the registered callback is null. Non-null error-only handlers count as implemented. Parser failures are errors, never dropped rows.",
        "",
        "Reference comparison is limited to command names, firmware notes and reference URLs already present in source comments. No external command catalog or measured game usage was supplied; missing commands absent from all source tables cannot be claimed complete. Links below identify available references; an empty note is no additional source-comment evidence, not proof the command is undocumented.",
        "",
        "## Runtime measurement",
        "",
        "Set `NXBOX_SERVICE_TRACE=1` before launching NXbox. On Xbox, place that line in `LocalState/nxbox_env.txt`; the existing frontend environment loader applies it. Ensure `LocalState/log_filter.txt` includes `Service:Info` (for example `*:Info Service:Info`) so INFO trace lines are retained. Service logs emit NXBOX_SERVICE_TRACE service=... protocol=CMIF/TIPC cmd=... result=0x... once per distinct service/protocol/command/result in the emulated System lifetime. Deferred or absent replies are marked explicitly, never decoded from request arguments. NXBOX_SERVICE_MISSING warns once per service/protocol/command even with tracing disabled; warnings include whether auto-stubbing was enabled. Existing auto-stub policy is retained and clearly labeled, so disable it for semantic validation. Trace output contains identifiers/results only, not guest buffers, tokens or account data. Use fresh System runs per title; compare independent runs for usage priority. Run `python3 tests/port/test_t16_service_coverage.py --trace-log /tmp/run1.log --trace-log /tmp/run2.log` to rank commands by independent run-log presence, with missing-run and result counts. Each input must represent one run; duplicate paths are deduplicated. Empty output means no trace evidence, not complete compatibility. No console usage ranking is claimed from static counts.",
        "",
        "## Validation and next console checks",
        "",
        "Port tests cover parser completeness, report freshness, baseline count and per-command implemented regressions in T16 ownership, real C++ trace deduplication/concurrency and NIFM lifecycle policies. MSVC full build remains separate from these portable checks.",
        "",
        "Console: collect boot/menu traces with auto-stub off; verify NIFM submit/cancel/resubmit, event wakeups, disabled wireless, wired LAN and link loss; check that Nintendo network-dependent paths fail gracefully. Account, friends, ns, nim, set, time, psm, audio, hid/irs, pctl, prepo, olsc, bcat, es, ldr and filesystem require measured traces before selecting additional commands. No title-specific workaround or Nintendo authentication is added.",
        "",
        "## Registered commands",
        "",
    ]
    duplicates = defaultdict(list)
    for command in commands:
        duplicates[command.identity].append(command)
    lines += [
        "## Duplicate registrations",
        "",
        "These entries are retained in the totals above. RegisterHandlersBase uses emplace_hint: the first registration of a duplicate ID wins. Source inventory has "
        + str(len(commands))
        + " rows and "
        + str(len(duplicates))
        + " distinct table/protocol/ID keys. Duplicate IDs are not additional reachable commands.",
        "",
    ]
    for identity, rows in duplicates.items():
        if len(rows) > 1:
            lines.append(
                "- " + "/".join(map(str, identity)) + ": " + ", ".join(row.name for row in rows)
            )
    lines += [
        "",
        "## T16 change summary",
        "",
        "Base ea3244c9c: all=1455 implemented / 539 stub-success / 4426 null; T16=1424 / 526 / 4359. This change fixes the NIFM ClientId buffer ABI; makes submit/cancel/resubmit and link-loss result stateful; ties accepted-request queries to client requests; enables three previously null network capability getters; separates wired communication policy from airplane mode; rejects unavailable current IP queries; adds wired-availability and disabled debug-emulation/rewrite getters. The request state model is synchronous for an already configured host interface; it does not bring up a host interface or validate WAN reachability. Event notifications for physical link loss are delivered when request state/result is polled; an asynchronous link monitor is future work.",
        "",
        "Semantic references: [libnx NIFM wire calls](https://github.com/switchbrew/libnx/blob/master/nx/source/services/nifm.c), [NIFM request and buffer descriptions](https://switchbrew.org/wiki/Network_Interface_services). These sources specify layouts and transitions, not real-game frequency.",
        "",
    ]
    for (source, table, interface, protocol), rows in groups.items():
        lines += [
            f"### {source} — {interface} / {table} / {protocol}",
            "",
            f"Counts: {totals(rows)}.",
            "",
        ]
        refs = sorted(
            set(ref for row in rows for ref in row.references if "switchbrew" in ref.lower())
        )
        if refs:
            lines += ["Source reference comments: " + "; ".join(f"<{ref}>" for ref in refs), ""]
        lines += [
            "| ID | Command | Handler | Classification | Source comment |",
            "| ---: | --- | --- | --- | --- |",
        ]
        for row in rows:
            cells = [str(row.command), row.name, row.handler, row.status, row.comment]
            lines.append("| " + " | ".join(cell.replace("|", "\\|") for cell in cells) + " |")
        lines.append("")
    return "\n".join(lines)


TRACE_LINE = re.compile(
    r"NXBOX_SERVICE_TRACE service=(\S+) protocol=(CMIF|TIPC) cmd=(\d+) result=(0x[0-9a-fA-F]+|deferred|no-response)"
)
MISSING_LINE = re.compile(r"NXBOX_SERVICE_MISSING service=(\S+) protocol=(CMIF|TIPC) cmd=(\d+)")


def rank_trace_runs(paths):
    """Count independent run logs that hit each pair, not repeated invocation frequency."""
    observations = defaultdict(lambda: {"runs": set(), "missing_runs": set(), "results": set()})
    for path in dict.fromkeys(Path(p).resolve() for p in paths):
        for line in path.read_text(errors="replace").splitlines():
            trace = TRACE_LINE.search(line)
            missing = MISSING_LINE.search(line)
            if trace or missing:
                match = trace or missing
                key = (match[1], match[2], int(match[3]))
                entry = observations[key]
                entry["runs"].add(str(path))
                if missing:
                    entry["missing_runs"].add(str(path))
                if trace:
                    entry["results"].add(trace[4])
    return [
        {
            "service": service,
            "protocol": protocol,
            "command": command,
            "runs": len(value["runs"]),
            "missing_runs": len(value["missing_runs"]),
            "results": sorted(value["results"]),
        }
        for (service, protocol, command), value in sorted(
            observations.items(),
            key=lambda item: (-len(item[1]["missing_runs"]), -len(item[1]["runs"]), item[0]),
        )
    ]


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.commands = inventory()

    def test_every_table_is_inventoried(self):
        self.assertGreater(len(self.commands), 4000)
        # Upstream tables contain duplicate IDs: retain and disclose those entries.
        self.assertGreaterEqual(len({c.identity for c in self.commands}), 6345)
        expected = sum(
            len(list(TABLE.finditer(mask_comments(p.read_text()))))
            + len(list(INLINE_TABLE.finditer(mask_comments(p.read_text()))))
            for p in SERVICE_ROOT.rglob("*")
            if p.suffix in (".cpp", ".h")
        )
        self.assertEqual(len({(c.source, c.table) for c in self.commands}), expected)
        self.assertNotIn("unknown", {c.interface for c in self.commands})

    def test_generated_report_is_current(self):
        self.assertEqual(REPORT.read_text(), render(self.commands))

    def test_implemented_count_does_not_decrease(self):
        # Filled from the base ea3244c9c before changing service behavior.
        self.assertGreaterEqual(
            sum(c.status == "implemented" for c in self.commands if owned(c)), BASELINE_IMPLEMENTED
        )

    def test_implemented_commands_do_not_regress(self):
        implemented = {c.identity for c in self.commands if owned(c) and c.status == "implemented"}
        baseline = {
            (*key.split("|"), command)
            for key, commands in BASELINE_COMMANDS.items()
            for command in commands
        }
        self.assertFalse(
            baseline - implemented, f"Regressed commands: {sorted(baseline - implemented)}"
        )

    def test_parser_wrappers_tipc_and_comments(self):
        text = """class Example : public ServiceFramework<Example> {
        Example() { static const FunctionInfo functions[] = {
          {0, D<&Example::Ready>, "Ready"}, // 18.0.0+
          {1, C<&Example::Stub>, "Stub"},
          {2, nullptr, "Absent"},
        }; RegisterHandlersTipc(functions); }
        Result Ready() { R_SUCCEED(); }
        Result Stub() { LOG_WARNING(Service, "(STUBBED) {called}"); R_SUCCEED(); }
        };"""
        rows = parse_tables(text, "example.cpp", methods(text))
        self.assertEqual(
            [r.status for r in rows], ["implemented", "stub-success", "nullptr-unimplemented"]
        )
        self.assertEqual(rows[0].comment, "18.0.0+")
        self.assertEqual(rows[0].protocol, "TIPC")

    def test_inline_registration_is_counted(self):
        text = 'SM::SM() { RegisterHandlers({{0, &SM::Initialize, "Initialize"}}); RegisterHandlersTipc({{1, nullptr, "Absent"}}); } void SM::Initialize() { return; }'
        rows = parse_tables(text, "sm/sm.cpp", methods(text))
        self.assertEqual(
            [(r.interface, r.protocol, r.command) for r in rows],
            [("SM", "CMIF", 0), ("SM", "TIPC", 1)],
        )

    def test_trace_ranking_counts_runs_not_repeated_lines(self):
        import tempfile

        with tempfile.TemporaryDirectory(dir="/tmp", prefix="nxbox-t16-trace-") as directory:
            first = Path(directory) / "first.log"
            second = Path(directory) / "second.log"
            first.write_text(
                "NXBOX_SERVICE_TRACE service=acc:u0 protocol=CMIF cmd=99 result=0x00000001\n" * 20
                + "NXBOX_SERVICE_MISSING service=acc:u0 protocol=CMIF cmd=99 name=Absent auto_stub=false\n"
            )
            second.write_text(
                "NXBOX_SERVICE_TRACE service=acc:u0 protocol=CMIF cmd=99 result=0x00000000\nNXBOX_SERVICE_TRACE service=acc:u1 protocol=TIPC cmd=99 result=deferred\n"
            )
            rows = rank_trace_runs([first, first, second])
            self.assertEqual(rows[0]["runs"], 2)
            self.assertEqual(rows[0]["missing_runs"], 1)
            self.assertEqual(rows[0]["results"], ["0x00000000", "0x00000001"])
            self.assertEqual(rows[1]["protocol"], "TIPC")

    def test_unrecognized_initializer_fails(self):
        with self.assertRaisesRegex(ValueError, "Unsupported table"):
            parse_tables(
                'FunctionInfo f[] = {{MAGIC, nullptr, "X"}}; RegisterHandlers(f);', "bad.cpp", {}
            )


BASELINE_IMPLEMENTED = 1424
BASELINE_COMMANDS = {
    "olsc/olsc_service_for_system_service.cpp|functions#1|CMIF": [0, 1, 2, 10000],
    "olsc/daemon_controller.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 12],
    "pm/pm.cpp|functions#1|CMIF": [0, 1],
    "pm/pm.cpp|functions#2|CMIF": [2, 4, 65000],
    "pm/pm.cpp|functions#3|CMIF": [0, 65000],
    "pm/pm.cpp|functions#4|CMIF": [6],
    "spl/spl_module.cpp|functions#1|CMIF": [0],
    "spl/spl.cpp|functions#1|CMIF": [0, 1, 5, 7, 11, 24, 25],
    "spl/spl.cpp|functions#2|CMIF": [0, 1, 5, 7, 11, 24, 25],
    "spl/spl.cpp|functions#3|CMIF": [0, 1, 5, 7, 11, 24, 25],
    "spl/spl.cpp|functions#4|CMIF": [0, 1, 5, 7, 11, 24, 25],
    "spl/spl.cpp|functions#5|CMIF": [0, 1, 5, 7, 11, 24, 25],
    "spl/spl.cpp|functions#6|CMIF": [0, 1, 5, 7, 11, 24, 25],
    "btm/btm_system_core.cpp|functions#1|CMIF": [4, 5, 6],
    "btm/btm_user.cpp|functions#1|CMIF": [0],
    "btm/btm_system.cpp|functions#1|CMIF": [0],
    "glue/ectx.cpp|functions#1|CMIF": [0],
    "glue/ectx.cpp|functions#4|CMIF": [0],
    "glue/bgtc.cpp|functions#1|CMIF": [100],
    "glue/arp.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "glue/arp.cpp|functions#2|CMIF": [0, 1, 2],
    "glue/arp.cpp|functions#3|CMIF": [0, 1],
    "glue/notif.cpp|functions#1|CMIF": [500, 510, 520, 530, 540, 1000],
    "glue/notif.cpp|functions#3|CMIF": [500, 510, 520, 530, 540, 1000],
    "sm/sm.cpp|inline_cmif#1|CMIF": [0, 1, 2, 3],
    "sm/sm.cpp|inline_tipc#1|TIPC": [0, 1, 2, 3],
    "sm/sm_controller.cpp|functions#1|CMIF": [0, 2, 3, 4, 5],
    "vi/application_display_service.cpp|functions#1|CMIF": [
        100,
        103,
        1010,
        1011,
        1020,
        1101,
        1102,
        2020,
        2021,
        2030,
        2031,
        2101,
        2102,
        2460,
        5202,
    ],
    "vi/manager_display_service.cpp|functions#1|CMIF": [2010, 2011, 6002],
    "vi/application_root_service.cpp|functions#1|CMIF": [0],
    "vi/system_display_service.cpp|functions#1|CMIF": [
        2204,
        2205,
        2207,
        8225,
        8254,
        8255,
        8256,
        8258,
    ],
    "vi/manager_root_service.cpp|functions#1|CMIF": [2],
    "vi/system_root_service.cpp|functions#1|CMIF": [1],
    "friend/friend_interface.cpp|functions#1|CMIF": [0, 1],
    "friend/friend.cpp|functions#1|CMIF": [0],
    "friend/friend.cpp|functions#2|CMIF": [0, 1, 2],
    "ssl/ssl.cpp|functions#1|CMIF": [0, 1, 2, 3, 5, 8, 9, 10, 11, 13, 21, 22, 23, 26, 27],
    "ssl/ssl.cpp|functions#2|CMIF": [2, 3],
    "ssl/ssl.cpp|functions#3|CMIF": [2, 3, 5],
    "nvdrv/nvdrv_interface.cpp|functions#1|CMIF": [0, 1, 2, 4, 9, 11, 12],
    "nvdrv/nvmemp.cpp|functions#1|CMIF": [0, 1],
    "npns/npns.cpp|functions#2|CMIF": [5, 8],
    "ncm/ncm.cpp|functions#4|CMIF": [0, 1, 2, 4, 5, 6],
    "ncm/ncm.cpp|functions#5|CMIF": [0, 2, 8, 15],
    "ncm/ncm.cpp|functions#7|CMIF": [4, 5],
    "ptm/psm.cpp|functions#1|CMIF": [0, 1, 2, 3, 4],
    "ptm/psm.cpp|functions#2|CMIF": [0, 1, 7, 17],
    "ptm/ts.cpp|functions#1|CMIF": [4],
    "ptm/ts.cpp|functions#2|CMIF": [1, 3, 4],
    "nim/nim.cpp|functions#1|CMIF": [0, 1, 2, 3, 5],
    "nim/nim.cpp|functions#2|CMIF": [0],
    "nim/nim.cpp|functions#7|CMIF": [0, 1, 2, 3, 4, 5],
    "nim/nim.cpp|functions#8|CMIF": [0],
    "sockets/bsd.cpp|functions#1|CMIF": [
        2,
        3,
        6,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        20,
        21,
        22,
        24,
        26,
        27,
        31,
    ],
    "sockets/nsd.cpp|functions#1|CMIF": [11, 20, 21, 100],
    "sockets/sfdnsres.cpp|functions#1|CMIF": [2, 5, 6, 10, 12],
    "apm/apm_interface.cpp|functions#1|CMIF": [0, 1],
    "apm/apm_interface.cpp|functions#2|CMIF": [0, 1],
    "apm/apm_interface.cpp|functions#3|CMIF": [1, 6, 7],
    "ns/service_getter_interface.cpp|functions#1|CMIF": [
        7988,
        7989,
        7991,
        7992,
        7993,
        7994,
        7995,
        7996,
        7997,
        7998,
        7999,
    ],
    "ns/read_only_application_control_data_interface.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "ns/read_only_application_control_data_interface.cpp|functions#2|CMIF": [
        0,
        1,
        2,
        5,
        13,
        19,
        23,
    ],
    "ns/platform_service_manager.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6],
    "ns/async_result.cpp|functions#1|CMIF": [1],
    "ns/query_service.cpp|functions#1|CMIF": [17, 18, 19],
    "ns/application_manager_interface.cpp|functions#1|CMIF": [
        0,
        4,
        5,
        16,
        40,
        43,
        47,
        48,
        55,
        59,
        71,
        221,
        400,
        407,
        408,
        419,
        1704,
    ],
    "ns/read_only_application_record_interface.cpp|functions#1|CMIF": [3],
    "prepo/prepo.cpp|functions#1|CMIF": [
        10100,
        10101,
        10102,
        10103,
        10104,
        10105,
        10106,
        10107,
        20100,
        20101,
        20102,
        20103,
    ],
    "psc/pm_service.cpp|functions#1|CMIF": [0],
    "lm/lm.cpp|functions#1|CMIF": [0, 1],
    "lm/lm.cpp|functions#2|CMIF": [0],
    "lbl/lbl.cpp|functions#1|CMIF": [
        2,
        3,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        22,
        24,
        25,
        26,
        27,
        28,
        29,
    ],
    "caps/caps_su.cpp|functions#1|CMIF": [203, 205],
    "caps/caps_ss.cpp|functions#1|CMIF": [203, 206],
    "caps/caps_a.cpp|functions#1|CMIF": [1, 3, 5, 101, 401, 1002, 1003],
    "caps/caps_u.cpp|functions#1|CMIF": [102, 142],
    "pctl/parental_control_service_factory.cpp|functions#1|CMIF": [0, 1],
    "pctl/parental_control_service.cpp|functions#1|CMIF": [
        1,
        1001,
        1004,
        1013,
        1031,
        1035,
        1061,
        1062,
        1063,
        1064,
        1065,
        1432,
        1457,
        1458,
        1459,
        1460,
        1473,
    ],
    "ngc/ngc.cpp|functions#4|CMIF": [452, 454],
    "ngc/ngc.cpp|functions#6|CMIF": [108, 208],
    "i2c/i2c.cpp|functions#1|CMIF": [10],
    "aoc/purchase_event_manager.cpp|functions#1|CMIF": [2, 3, 4],
    "aoc/addon_content_manager.cpp|functions#1|CMIF": [2, 3, 5],
    "ro/ro.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 10],
    "usb/usb.cpp|functions#7|CMIF": [0],
    "usb/usb.cpp|functions#9|CMIF": [0],
    "pcv/pcv.cpp|functions#5|CMIF": [0],
    "audio/audio_out_manager.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "audio/audio_out.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13],
    "audio/audio_controller.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        9,
        10,
        14,
        18,
        30,
        31,
        32,
        5000,
    ],
    "audio/audio_in_manager.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5],
    "audio/hardware_opus_decoder_manager.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
    "audio/audio_renderer_manager.cpp|functions#1|CMIF": [0, 1, 2, 4],
    "audio/hardware_opus_decoder.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
    "audio/audio_device.cpp|functions#1|CMIF": [0, 1, 2, 3, 6, 7, 8, 12, 13, 14],
    "audio/audio_renderer.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13],
    "audio/audio_in.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
    "jit/jit.cpp|functions#1|CMIF": [0, 1, 1000, 1001],
    "jit/jit.cpp|functions#2|CMIF": [0],
    "nfc/nfc.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        400,
        401,
        402,
        403,
        404,
        405,
        406,
        407,
        408,
        409,
        410,
        411,
        412,
        1000,
        1001,
    ],
    "nfc/nfc.cpp|functions#2|CMIF": [
        0,
        1,
        2,
        3,
        100,
        400,
        401,
        402,
        403,
        404,
        405,
        406,
        407,
        408,
        409,
        410,
        411,
        412,
        500,
        1000,
        1001,
    ],
    "nfc/nfc.cpp|functions#3|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13],
    "nfc/nfc.cpp|functions#5|CMIF": [0],
    "nfc/nfc.cpp|functions#6|CMIF": [0],
    "nfc/nfc.cpp|functions#7|CMIF": [0],
    "nfc/nfc.cpp|functions#8|CMIF": [0],
    "nifm/nifm.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "nifm/nifm.cpp|functions#2|CMIF": [9],
    "nifm/nifm.cpp|functions#4|CMIF": [2, 4, 5, 6, 7, 8, 9, 11, 14, 15, 16, 17, 18, 19, 35, 36, 47],
    "nifm/nifm.cpp|functions#5|CMIF": [4, 5],
    "nfp/nfp.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        22,
        23,
        24,
        25,
    ],
    "nfp/nfp.cpp|functions#2|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        23,
        25,
        100,
        101,
        102,
        103,
        104,
        105,
        106,
    ],
    "nfp/nfp.cpp|functions#3|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        22,
        23,
        24,
        25,
        100,
        101,
        102,
        103,
        104,
        105,
        106,
        200,
        201,
        202,
        203,
        204,
        205,
        206,
    ],
    "nfp/nfp.cpp|functions#4|CMIF": [0],
    "nfp/nfp.cpp|functions#5|CMIF": [0],
    "nfp/nfp.cpp|functions#6|CMIF": [0],
    "hid/hid_server.cpp|functions#1|CMIF": [
        0,
        1,
        11,
        21,
        31,
        55,
        59,
        66,
        67,
        68,
        69,
        70,
        71,
        72,
        79,
        80,
        81,
        82,
        83,
        84,
        85,
        87,
        88,
        89,
        91,
        100,
        101,
        102,
        103,
        104,
        106,
        107,
        108,
        109,
        120,
        121,
        122,
        123,
        124,
        125,
        126,
        127,
        128,
        129,
        130,
        131,
        132,
        133,
        134,
        135,
        136,
        200,
        201,
        202,
        203,
        204,
        205,
        206,
        207,
        208,
        209,
        210,
        211,
        212,
        300,
        303,
        306,
        310,
        500,
        501,
        503,
        504,
        505,
        506,
        507,
        508,
        517,
        519,
        1002,
        1004,
    ],
    "hid/applet_resource.cpp|functions#1|CMIF": [0],
    "hid/active_vibration_device_list.cpp|functions#1|CMIF": [0],
    "hid/hid_debug_server.cpp|functions#1|CMIF": [10, 11, 12, 13, 14, 15, 16, 17, 91],
    "hid/hid_system_server.cpp|functions#1|CMIF": [
        161,
        303,
        304,
        305,
        306,
        308,
        310,
        312,
        313,
        315,
        322,
        501,
        502,
        503,
        504,
        505,
        506,
        507,
        510,
        511,
        512,
        513,
        751,
        1150,
        1151,
        1152,
        1153,
        1155,
    ],
    "hid/hidbus.cpp|functions#1|CMIF": [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12],
    "hid/irs.cpp|functions#1|CMIF": [304, 308, 311, 316],
    "bcat/delivery_cache_progress_service.cpp|functions#1|CMIF": [0, 1],
    "bcat/delivery_cache_file_service.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "bcat/service_creator.cpp|functions#1|CMIF": [0, 1, 2],
    "bcat/delivery_cache_storage_service.cpp|functions#1|CMIF": [0, 1, 10],
    "bcat/delivery_cache_directory_service.cpp|functions#1|CMIF": [0, 1, 2],
    "bcat/bcat_service.cpp|functions#1|CMIF": [10100, 10101, 30100, 90201],
    "es/es.cpp|functions#1|CMIF": [1, 8, 9, 10, 11, 12, 14, 15, 16, 17],
    "nvnflinger/hos_binder_driver.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "set/system_settings_server.cpp|functions#1|CMIF": [
        0,
        3,
        4,
        7,
        8,
        13,
        14,
        15,
        16,
        17,
        18,
        21,
        22,
        23,
        24,
        25,
        26,
        27,
        28,
        29,
        30,
        31,
        32,
        35,
        36,
        37,
        38,
        39,
        40,
        43,
        44,
        45,
        46,
        47,
        48,
        53,
        54,
        57,
        58,
        59,
        60,
        61,
        62,
        63,
        64,
        65,
        66,
        67,
        68,
        69,
        70,
        71,
        72,
        73,
        74,
        75,
        76,
        77,
        78,
        88,
        89,
        90,
        95,
        96,
        99,
        100,
        105,
        106,
        120,
        121,
        124,
        125,
        126,
        127,
        136,
        137,
        150,
        151,
        152,
        153,
        170,
        187,
        188,
        201,
        203,
        204,
    ],
    "set/settings_server.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12],
    "fgm/fgm.cpp|functions#2|CMIF": [0],
    "mii/mii.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        20,
        21,
        22,
        23,
        24,
        25,
        26,
    ],
    "mii/mii.cpp|functions#2|CMIF": [0],
    "mii/mii.cpp|functions#3|CMIF": [0, 11],
    "fatal/fatal_u.cpp|functions#1|CMIF": [0, 1, 2],
    "acc/async_context.cpp|functions#1|CMIF": [0, 1, 2, 3],
    "acc/acc.cpp|functions#1|CMIF": [100],
    "acc/acc.cpp|functions#2|CMIF": [2, 3, 4, 120, 130, 136],
    "acc/acc.cpp|functions#4|CMIF": [0, 1, 2, 3, 120, 130, 250],
    "acc/acc.cpp|functions#10|CMIF": [0, 1, 10, 11],
    "acc/acc.cpp|editor_functions#1|CMIF": [100, 101],
    "acc/acc.cpp|functions#13|CMIF": [1, 3, 160, 170],
    "acc/acc.cpp|functions#18|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        51,
        52,
        60,
        102,
        110,
        140,
        200,
        201,
        203,
        204,
        205,
        250,
    ],
    "acc/acc.cpp|functions#19|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        51,
        52,
        60,
        100,
        101,
        110,
        131,
        140,
        141,
        150,
    ],
    "acc/acc.cpp|functions#20|CMIF": [0, 1, 2, 3, 4, 5, 51, 52, 60, 102, 110, 140],
    "bcat/news/overwrite_event_holder.cpp|functions#1|CMIF": [0],
    "bcat/news/service_creator.cpp|functions#1|CMIF": [0, 1, 2, 3, 4],
    "bcat/news/news_service.cpp|functions#1|CMIF": [30101, 30110],
    "bcat/news/news_data_service.cpp|functions#1|CMIF": [0, 1, 2, 3, 1001],
    "bcat/news/newly_arrived_event_holder.cpp|functions#1|CMIF": [0],
    "bcat/news/news_database_service.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 1000],
    "filesystem/fsp/fs_i_directory.cpp|functions#1|CMIF": [0, 1],
    "filesystem/fsp/fsp_srv.cpp|functions#1|CMIF": [
        1,
        2,
        18,
        22,
        23,
        37,
        51,
        52,
        53,
        61,
        67,
        200,
        202,
        203,
        205,
        210,
        1003,
        1004,
        1005,
        1006,
        1200,
    ],
    "filesystem/fsp/fs_i_save_data_info_reader.cpp|functions#1|CMIF": [0],
    "filesystem/fsp/fs_i_filesystem.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        11,
        12,
        13,
        14,
    ],
    "filesystem/fsp/fs_i_file.cpp|functions#1|CMIF": [0, 1, 2, 3, 4],
    "filesystem/fsp/fs_i_storage.cpp|functions#1|CMIF": [0, 4],
    "psc/ovln/receiver_service.cpp|functions#1|CMIF": [0],
    "psc/ovln/receiver.cpp|functions#1|CMIF": [0, 1, 2, 3, 4],
    "psc/time/time_zone_service.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        20,
        100,
        101,
        201,
        202,
    ],
    "psc/time/service_manager.cpp|functions#1|CMIF": [
        0,
        5,
        6,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        50,
        51,
        52,
        60,
        100,
        200,
        201,
        202,
    ],
    "psc/time/steady_clock.cpp|functions#1|CMIF": [0, 2, 3, 100, 101, 102, 200],
    "psc/time/power_state_service.cpp|functions#1|CMIF": [0, 1],
    "psc/time/static.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        20,
        50,
        51,
        100,
        101,
        102,
        200,
        201,
        300,
        400,
        401,
        500,
        501,
    ],
    "psc/time/alarms.cpp|functions#1|CMIF": [0, 1],
    "psc/time/alarms.cpp|functions#2|CMIF": [0, 1, 2, 3],
    "psc/time/system_clock.cpp|functions#1|CMIF": [0, 1, 2, 3, 4],
    "am/service/application_functions.cpp|functions#1|CMIF": [
        1,
        12,
        20,
        21,
        23,
        25,
        26,
        29,
        121,
        122,
        140,
        141,
        150,
        160,
        210,
        330,
    ],
    "am/service/window_controller.cpp|functions#1|CMIF": [1, 2, 10, 11, 12, 20],
    "am/service/global_state_controller.cpp|functions#1|CMIF": [3, 4, 14, 15, 30],
    "am/service/lock_accessor.cpp|functions#1|CMIF": [1, 2, 3, 4],
    "am/service/application_creator.cpp|functions#1|CMIF": [0, 10],
    "am/service/library_applet_creator.cpp|functions#1|CMIF": [0, 3, 10, 11, 12],
    "am/service/overlay_functions.cpp|functions#1|CMIF": [0, 1, 2, 20, 21, 31, 70],
    "am/service/storage.cpp|functions#1|CMIF": [0, 1],
    "am/service/library_applet_self_accessor.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        5,
        6,
        10,
        11,
        14,
        15,
        17,
        30,
        60,
        110,
        160,
    ],
    "am/service/audio_controller.cpp|functions#1|CMIF": [0, 1, 2, 3, 4],
    "am/service/application_accessor.cpp|functions#1|CMIF": [
        0,
        10,
        20,
        25,
        30,
        101,
        121,
        122,
        130,
        150,
    ],
    "am/service/process_winding_controller.cpp|functions#1|CMIF": [0, 11, 21, 22, 23, 30, 40, 41],
    "am/service/system_applet_proxy.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        10,
        11,
        20,
        21,
        22,
        23,
        1000,
    ],
    "am/service/library_applet_accessor.cpp|functions#1|CMIF": [
        0,
        1,
        10,
        20,
        25,
        30,
        100,
        101,
        103,
        104,
        105,
        106,
        120,
    ],
    "am/service/library_applet_proxy.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        10,
        11,
        20,
        21,
        22,
        23,
        1000,
    ],
    "am/service/application_proxy.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 10, 11, 20, 1000],
    "am/service/display_controller.cpp|functions#1|CMIF": [20, 22, 24, 26],
    "am/service/home_menu_functions.cpp|functions#1|CMIF": [10, 11, 12, 20, 21, 40, 41, 110],
    "am/service/self_controller.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        10,
        11,
        12,
        13,
        14,
        16,
        40,
        41,
        42,
        43,
        44,
        68,
        69,
        90,
        91,
        120,
    ],
    "am/service/all_system_applet_proxies_service.cpp|functions#1|CMIF": [200],
    "am/service/overlay_applet_proxy.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        10,
        11,
        20,
        21,
        23,
        1000,
    ],
    "am/service/common_state_getter.cpp|functions#1|CMIF": [
        0,
        1,
        5,
        6,
        8,
        9,
        13,
        20,
        30,
        31,
        32,
        50,
        51,
        55,
        60,
        61,
        62,
        63,
        66,
        80,
        100,
        120,
        300,
        1000,
        1001,
    ],
    "am/service/storage_accessor.cpp|functions#1|CMIF": [0, 10, 11],
    "am/service/storage_accessor.cpp|functions#2|CMIF": [0, 1],
    "glue/time/static.cpp|functions#1|CMIF": [
        0,
        1,
        2,
        3,
        4,
        5,
        20,
        50,
        51,
        100,
        101,
        102,
        200,
        201,
        300,
        400,
        401,
        500,
        501,
    ],
    "glue/time/time_zone.cpp|functions#1|CMIF": [0, 1, 2, 3, 4, 5, 6, 7, 8, 20, 100, 101, 201, 202],
}

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--trace-log", action="append", default=[])
    args, rest = parser.parse_known_args()
    if args.trace_log:
        print(
            json.dumps(
                {
                    "metric": "independent_runs_with_command_not_invocation_frequency",
                    "commands": rank_trace_runs(args.trace_log),
                },
                indent=2,
            )
        )
    elif args.generate or args.summary:
        commands = inventory()
        print(f"All: {totals(commands)}; total={len(commands)}")
        print(f"T16: {totals([c for c in commands if owned(c)])}")
        if args.generate:
            REPORT.write_text(render(commands))
    else:
        unittest.main(argv=[__file__, *rest])
