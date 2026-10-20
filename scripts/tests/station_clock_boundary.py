#!/usr/bin/env python3
"""Keep Station translation units (including their private headers) World-clock free."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
forbidden = re.compile(r"\bgetFixedTimestep\b|#\s*include\s*[<\"]core/World\.h[>\"]")
violations = []
for path in sorted((root / "src/core").glob("Station*")):
    if path.suffix not in {".cpp", ".h"}:
        continue
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if forbidden.search(line):
            violations.append(f"{path.relative_to(root)}:{number}: {line.strip()}")
if violations:
    raise SystemExit("Station code must use systems time, not the World timestep:\n" + "\n".join(violations))
print("Station clock boundary: OK")
