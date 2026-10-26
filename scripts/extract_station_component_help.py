#!/usr/bin/env python3
"""Extract the component reference into the editor's structured JSON help resource.

Run without arguments to regenerate; --check verifies that the resource is current.
Only the Python standard library is needed. Inline Markdown is converted to plain
text; headings, lists and dial tables remain structured for native UI rendering.
"""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs/station-components.md"
DESTINATION = ROOT / "resources/station-systems/component-help.json"


def plain(text):
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    return re.sub(r"\*\*|`", "", text).strip()


def blocks(text):
    result = []
    paragraphs = re.split(r"\n\s*\n", text.strip())
    for paragraph in paragraphs:
        lines = paragraph.splitlines()
        if lines[0].startswith("| "):
            rows = [[plain(cell) for cell in line.strip().strip("|").split("|")]
                    for line in lines]
            assert len(rows[0]) == 2 and all(len(row) == 2 for row in rows)
            result.append({"kind": "settings", "rows": [
                {"name": row[0], "description": row[1]} for row in rows[2:]]})
        elif lines[0].startswith("- "):
            items = re.split(r"\n- ", paragraph[2:])
            result.append({"kind": "list", "items": [plain(" ".join(item.split())) for item in items]})
        else:
            text = " ".join(line.strip() for line in lines)
            # Labelled paragraphs become native section headings, never raw Markdown.
            match = re.match(r"\*\*([^*]+):\*\*\s*(.*)", text)
            if match:
                result.append({"kind": "heading", "text": plain(match[1])})
                text = match[2]
            result.append({"kind": "paragraph", "text": plain(text)})
    return result


def extract(text):
    text = text.split("## Worked system arrangements", 1)[0]
    components = []
    shared_biology = ""
    for match in re.finditer(r"^### ([^\n]+)\n(.*?)(?=^#{1,3} |\Z)", text, re.M | re.S):
        title, body = match.groups()
        if title == "Shared biological requirements":
            shared_biology = body
            continue
        if title in {"Ports, services and actual operation", "Units and material requirements", "Shared dials"}:
            continue
        node_type = {"Power (shared electrical pool)": "Power", "Prefabs": "Prefab",
                     "Station settings and monitoring": "Station settings"}.get(title, title)
        entry = {"nodeType": node_type, "title": title, "blocks": blocks(body)}
        if node_type in {"Crew", "Crops", "Algae", "Microbial processor"}:
            assert shared_biology
            entry["blocks"] += [{"kind": "heading", "text": "Shared biological requirements"}] + blocks(shared_biology)
        components.append(entry)
    assert len({entry["nodeType"] for entry in components}) == len(components)
    assert len(components) == 38, f"Unexpected component count: {len(components)}"
    return {"version": 1, "source": "docs/station-components.md", "components": components}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = json.dumps(extract(SOURCE.read_text(encoding="utf-8")), ensure_ascii=False, indent=2) + "\n"
    if args.check:
        if not DESTINATION.exists() or DESTINATION.read_text(encoding="utf-8") != content:
            raise SystemExit("Component help is stale; run scripts/extract_station_component_help.py")
        print("Station component help is current.")
    else:
        DESTINATION.write_text(content, encoding="utf-8")
        print(f"Wrote {DESTINATION.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
