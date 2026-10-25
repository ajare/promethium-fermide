#!/usr/bin/env python3
"""Require an explicit coverage classification for every public snapshot member.

This checks catalogue completeness, not physical fidelity. Unverified entries
remain permissible and must not be mistaken for passing convergence evidence.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
def snapshot_members(header):
    header = re.sub(r"//[^\n]*|/\*.*?\*/", "", header, flags=re.DOTALL)
    # Require the definition's opening brace, never a forward declaration's
    # next brace (which may belong to an entirely different struct).
    definition = re.search(r"\bstruct\s+StationSystemsSnapshot\s*\{", header)
    if not definition:
        raise SystemExit("Snapshot definition not found")
    start = definition.end()
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
        # A single type may declare several members: `std::vector<X> active, closed`.
        # Split the declarators off the type, then the initializer off each one.
        match = re.fullmatch(r"([\w:]+(?:<.*>)?)\s+(.+)", declaration, re.DOTALL)
        if not match:
            raise SystemExit(f"Unrecognized snapshot declaration; update the coverage check: {declaration}")
        for declarator in match.group(2).split("{")[0].split(","):
            declarator = declarator.strip()
            if not re.fullmatch(r"\w+", declarator):
                raise SystemExit(f"Unrecognized snapshot declaration; update the coverage check: {declaration}")
            members.add(declarator)
    return members


def main():
    members = snapshot_members((ROOT / "include/core/StationSystems.h").read_text(encoding="utf-8"))
    source = (ROOT / "src/core/StationObservableInvariance.cpp").read_text(encoding="utf-8")
    match = re.search(r"std::array<std::string_view,\s*\d+>\s+names\s*\{(.*?)\};", source, re.DOTALL)
    if not match:
        raise SystemExit("Observable coverage inventory not found")
    names = re.findall(r'"([A-Za-z_]\w*)"', match.group(1))
    if len(names) != len(set(names)) or set(names) != members:
        raise SystemExit(f"Snapshot coverage mismatch: missing={sorted(members - set(names))}, "
                         f"obsolete={sorted(set(names) - members)}, duplicate={len(names) != len(set(names))}")
    print(f"All {len(members)} snapshot members have an explicit coverage classification; fidelity is checked separately.")


if __name__ == "__main__":
    main()
