#!/usr/bin/env python3
"""Keep Station smoke checks expressed in simulated seconds, not World ticks."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
sources = root / "src/headless/smoke/station"
# A direct advanceTicks/advanceTick call makes its enclosing require assertion
# depend on the World clock.  StationRig owns the sole fixture-boundary
# conversion used by advanceSeconds().
forbidden = re.compile(r"(?:\.|->)advanceTicks?\s*\(")
violations = []
for path in sorted(sources.glob("*.cpp")):
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if forbidden.search(line) and not line.lstrip().startswith("//"):
            violations.append(f"{path.relative_to(root)}:{number}: {line.strip()}")
if violations:
    raise SystemExit(
        "Station checks must assert simulated seconds via advanceSeconds(), not raw World ticks:\n"
        + "\n".join(violations)
    )
print("Station seconds lint: OK")
