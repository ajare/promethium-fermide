#!/usr/bin/env python3
"""Require an explicit coverage classification for every public snapshot member.

This checks catalogue completeness, not physical fidelity. Unverified entries
remain permissible and must not be mistaken for passing convergence evidence.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
header = (ROOT / "include/core/StationSystems.h").read_text(encoding="utf-8")
header = re.sub(r"//[^\n]*|/\*.*?\*/", "", header, flags=re.DOTALL)
start = header.index("{", header.index("struct StationSystemsSnapshot")) + 1
depth = 1
end = start
while depth:
    depth += (header[end] == "{") - (header[end] == "}")
    end += 1
members = set()
for declaration in header[start:end - 1].split(";"):
    declaration = declaration.strip()
    if not declaration or "operator==" in declaration:
        continue
    match = re.fullmatch(r"[\w:]+(?:<.*>)?\s+(\w+)\s*(?:\{.*\})?", declaration, re.DOTALL)
    if not match:
        raise SystemExit(f"Unrecognized snapshot declaration; update the coverage check: {declaration}")
    members.add(match.group(1))

source = (ROOT / "src/core/StationObservableInvariance.cpp").read_text(encoding="utf-8")
match = re.search(r"std::array<std::string_view,\s*\d+>\s+names\s*\{(.*?)\};", source, re.DOTALL)
if not match:
    raise SystemExit("Observable coverage inventory not found")
names = re.findall(r'"([A-Za-z_]\w*)"', match.group(1))
if len(names) != len(set(names)) or set(names) != members:
    raise SystemExit(f"Snapshot coverage mismatch: missing={sorted(members - set(names))}, "
                     f"obsolete={sorted(set(names) - members)}, duplicate={len(names) != len(set(names))}")
print(f"All {len(members)} snapshot members have an explicit coverage classification; fidelity is checked separately.")
