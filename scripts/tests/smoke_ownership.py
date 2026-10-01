"""Audit exactly-once ownership for every modular and standalone smoke source."""

from __future__ import annotations

import pathlib
import re
import sys
from collections import Counter


if len(sys.argv) != 4:
    raise SystemExit("usage: smoke_ownership.py <source-root> <smoke-cmake> <manifest>")

source_root = pathlib.Path(sys.argv[1]).resolve()
smoke_root = source_root / "src/headless/smoke"
cmake_text = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
manifest_text = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")

blocks = re.findall(r"pf_add_smoke_module\((pf-smoke-[^\s)]+)(.*?)\n\s*\)", cmake_text, re.DOTALL)
cmake_owners: dict[str, str] = {}
errors: list[str] = []
for target, body in blocks:
    match = re.search(r"\bSOURCES\b(.*?)\b(?:LIBRARIES|LABELS|TIMEOUT)\b", body, re.DOTALL)
    if not match:
        errors.append(f"{target}: SOURCES list is missing")
        continue
    for token in re.findall(r"[^\s]+\.cpp", match.group(1)):
        if pathlib.PurePosixPath(token).name in {"Main.cpp", "State.cpp"}:
            continue
        if token in cmake_owners:
            errors.append(f"{token}: CMake owners {cmake_owners[token]} and {target}")
        cmake_owners[token] = target

actual_module_sources = {
    path.relative_to(smoke_root).as_posix()
    for path in smoke_root.rglob("*.cpp")
    if path.parent.name not in {"support", "tests"}
    and path.name not in {"Main.cpp", "State.cpp"}
}
for source in sorted(actual_module_sources - cmake_owners.keys()):
    errors.append(f"{source}: smoke check source has no CMake owner")
for source in sorted(cmake_owners.keys() - actual_module_sources):
    errors.append(f"{source}: CMake-owned smoke check source does not exist")

standalone_owners = {
    "OccupantPackingChecks.cpp": "pf-occupant-packing-checks",
    "SectorTilesetChecks.cpp": "pf-sector-tileset-checks",
    "SimulationStepTimingChecks.cpp": "pf-simulation-step-timing-checks",
    "WorldRenderSlotChecks.cpp": "pf-world-render-slot-checks",
}
expected_owners = {**cmake_owners, **standalone_owners}

manifest_rows: list[tuple[str, str]] = []
in_current_table = False
for line in manifest_text.splitlines():
    if line == "<!-- current-ownership-begin -->":
        in_current_table = True
        continue
    if line == "<!-- current-ownership-end -->":
        in_current_table = False
        continue
    if not in_current_table:
        continue
    match = re.match(r"\| `([^`]+)` \| [^|]+ \| `([^`]+)`", line)
    if match:
        source, target = match.groups()
        if source.startswith("smoke/"):
            source = source.removeprefix("smoke/")
        manifest_rows.append((source, target))

counts = Counter(source for source, _ in manifest_rows)
for source, count in sorted(counts.items()):
    if count != 1:
        errors.append(f"{source}: manifest contains {count} current ownership rows")
manifest_owners = dict(manifest_rows)
for source in sorted(expected_owners.keys() - manifest_owners.keys()):
    errors.append(f"{source}: missing from current ownership manifest")
for source in sorted(manifest_owners.keys() - expected_owners.keys()):
    errors.append(f"{source}: manifest row is not a current smoke check source")
for source in sorted(expected_owners.keys() & manifest_owners.keys()):
    if expected_owners[source] != manifest_owners[source]:
        errors.append(
            f"{source}: manifest owner {manifest_owners[source]}, CMake owner {expected_owners[source]}"
        )

if errors:
    for error in errors:
        print(f"ERROR ownership: {error}", file=sys.stderr)
    raise SystemExit(1)
print(
    f"PASS smoke ownership: {len(cmake_owners)} modular sources and "
    f"{len(standalone_owners)} standalone sources each have one owner"
)
