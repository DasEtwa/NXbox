#!/usr/bin/env python3
"""Apply reproducible platform settings after `tauri android/ios init`."""

from pathlib import Path
import json
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
android = root / "src-tauri/gen/android"
if android.exists():
    manifest = android / "app/src/main/AndroidManifest.xml"
    namespace = "http://schemas.android.com/apk/res/android"
    ET.register_namespace("android", namespace)
    tree = ET.parse(manifest)
    document = tree.getroot()
    for name in ("INTERNET", "ACCESS_NETWORK_STATE"):
        permission = f"android.permission.{name}"
        if not any(
            item.get(f"{{{namespace}}}name") == permission
            for item in document.findall("uses-permission")
        ):
            ET.SubElement(document, "uses-permission", {f"{{{namespace}}}name": permission})
    document.find("application").set(f"{{{namespace}}}allowBackup", "false")
    tree.write(manifest, encoding="utf-8", xml_declaration=True)

apple = root / "src-tauri/gen/apple"
if apple.exists():
    project = apple / "project.yml"
    text = project.read_text()
    marker = "      - path: Sources"
    if "path: Localizations" not in text:
        # Only the concrete iOS target receives resources; leave target templates untouched.
        target = text.index("targets:")
        before, after = text[:target], text[target:]
        after = after.replace(marker, marker + "\n      - path: Localizations", 1)
        text = before + after
    project.write_text(text)
    messages = {
        "en": "NxbHelper finds your Xbox and sends games, keys and firmware over your home network.",
        "pt-BR": "O NxbHelper encontra seu Xbox e envia jogos, chaves e firmware pela rede de casa.",
    }
    for language, message in messages.items():
        directory = apple / "Localizations" / f"{language}.lproj"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "InfoPlist.strings").write_text(
            '"NSLocalNetworkUsageDescription" = ' + json.dumps(message, ensure_ascii=False) + ";\n"
        )
