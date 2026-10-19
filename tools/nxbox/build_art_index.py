#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the keyless LaunchBox Switch background index; never download image bytes."""

import argparse
import json
from pathlib import Path
import re
import shutil
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

URL = "https://gamesdb.launchbox-app.com/Metadata.zip"
USER_AGENT = "NXbox art index (+https://github.com/NspxMiguel/NXbox)"
SUFFIXES = (
    "definitive edition",
    "deluxe edition",
    "complete edition",
    "ultimate edition",
    "game of the year edition",
    "special edition",
    "standard edition",
    "gold edition",
    "digital deluxe edition",
    "collectors edition",
    "anniversary edition",
    "remastered",
)
FOLD = dict(zip("àáâãäåèéêëìíîïòóôõöùúûüýÿçñ", "aaaaaaeeeeiiiiooooouuuuyycn"))


def normalize(name):
    # Keep non-Latin scripts; match the portable console normalizer exactly.
    name = "".join(
        c.lower() if "A" <= c <= "Z" or "À" <= c <= "Þ" and c != "×" else c for c in name
    )
    name = "".join(FOLD.get(c, c) for c in name)
    name = re.sub(r"[\u2000-\u206f\u3000-\u303f]", " ", name)
    name = name.translate(str.maketrans("", "", "™®©"))
    name = " ".join(re.sub(r"[^a-z0-9\u0080-\uffff]+", " ", name).split())
    changed = True
    while changed:
        changed = False
        for suffix in sorted(SUFFIXES, key=len, reverse=True):
            if name.endswith(" " + suffix):
                name = name[: -len(suffix)].strip()
                changed = True
                break
    return " ".join(word for word in name.split() if word != "the")


def safe_filename(name):
    return bool(re.fullmatch(r"[A-Za-z0-9_-]+\.(?:jpg|jpeg|png)", name, re.I))


def records(xml, tag):
    events = ET.iterparse(xml, events=("start", "end"))
    _, root = next(events)
    for event, element in events:
        if event == "end" and element.tag == tag:
            record = {child.tag: child.text or "" for child in element}
            element.clear()
            root.clear()
            yield record
        elif event == "end" and element.tag in ("Game", "GameImage"):
            element.clear()
            root.clear()


def build_index(xml):
    # Two streaming passes keep only Switch records, even when images precede games.
    games = {
        record.get("DatabaseID", ""): normalize(record.get("Name", ""))
        for record in records(xml, "Game")
        if record.get("Platform") == "Nintendo Switch"
    }
    if hasattr(xml, "seek"):
        xml.seek(0)
    merged = {}
    for record in records(xml, "GameImage"):
        name = games.get(record.get("DatabaseID", ""))
        kind = record.get("Type")
        filename = record.get("FileName", "")
        if name and kind in ("Fanart - Background", "Banner") and safe_filename(filename):
            region = record.get("Region", "")
            rank = (
                kind != "Fanart - Background",
                region not in ("", "World"),
                region != "North America",
                region != "Europe",
                filename,
            )
            merged.setdefault(name, []).append((rank, filename))
    return {
        name: list(dict.fromkeys(filename for _, filename in sorted(merged[name])))
        for name in sorted(merged)
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, help="Local Metadata.xml or Metadata.zip")
    parser.add_argument("--output", type=Path, default=Path("dist/art/backgrounds.json"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nxbox-launchbox-") as temporary:
        source = args.metadata
        if source is None:
            source = Path(temporary) / "Metadata.zip"
            # The host answers 403 to urllib's default User-Agent.
            request = urllib.request.Request(URL, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=120) as response, source.open("wb") as out:
                shutil.copyfileobj(response, out)
        if zipfile.is_zipfile(source):
            with zipfile.ZipFile(source) as archive:
                member = next(n for n in archive.namelist() if n.lower() == "metadata.xml")
                with archive.open(member) as xml:
                    index = build_index(xml)
        else:
            index = build_index(source)
        if not index:
            raise SystemExit("No Switch background images found; preserving existing output")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        partial = args.output.with_suffix(".json.partial")
        partial.write_text(
            json.dumps(index, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8"
        )
        partial.replace(args.output)
        print(f"Wrote {len(index)} titles to {args.output}")


if __name__ == "__main__":
    main()
