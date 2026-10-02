#!/usr/bin/env python3
"""Validate #309 in an existing MSVC tree, in Debug and Release, without GUI runs."""
import argparse
import json
from pathlib import Path
import sys

from validate_windows_smoke import run

AGGREGATE = "prometheum-fermide-headless"
TOOLS = {"pf-restoration-benchmark", "pf-generate-routing-world", "pf-metrics-server",
         "pf-lift-repro", "pf-pause-position-repro", "pf-compatibility-probe"}
SYSTEM_LIBRARIES = {name + ".lib" for name in (
    "kernel32", "user32", "gdi32", "winspool", "shell32", "ole32", "oleaut32",
    "uuid", "comdlg32", "advapi32")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-tree", required=True, type=Path)
    parser.add_argument("--parallel", type=int, default=4)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("this validation requires Windows and MSVC")
    build = args.build_tree.resolve()
    if not (build / "CMakeCache.txt").exists():
        parser.error("configure an MSVC build tree with BUILD_TESTING=ON first")
    logs = build / "compatibility-validation-logs"
    logs.mkdir(exist_ok=True)
    query = build / ".cmake/api/v1/query/codemodel-v2"
    query.parent.mkdir(parents=True, exist_ok=True)
    query.touch()
    source = Path(__file__).resolve().parents[1]
    run(["cmake", "-S", str(source), "-B", str(build)], logs / "configure.log")
    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json")).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    if {config["name"] for config in model["configurations"]} != {"Debug", "Release"}:
        raise RuntimeError("expected MSVC Debug/Release multi-configuration tree")
    # Read snapshots before building can regenerate and prune the File API reply.
    snapshots = {config["name"]: [json.loads((reply / ref["jsonFile"]).read_text())
                                   for ref in config["targets"]]
                 for config in model["configurations"]}
    for config, targets in snapshots.items():
        by_name = {target["name"]: target for target in targets}
        by_id = {target["id"]: target for target in targets}
        aggregate = by_name[AGGREGATE]
        sources = [item["path"] for item in aggregate["sources"] if "compileGroupIndex" in item]
        if sources != ["src/headless/SmokeScenario.cpp"]:
            raise RuntimeError(f"{config}: compatibility compiles unexpected sources: {sources}")
        libraries = {token.lower() for fragment in aggregate["link"]["commandFragments"]
                     if fragment["role"] == "libraries" for token in fragment["fragment"].split()}
        if libraries - SYSTEM_LIBRARIES:
            raise RuntimeError(f"{config}: compatibility links non-system libraries: {libraries}")
        boundaries = {}
        for name, target in by_name.items():
            if not name.startswith("pf-smoke-") or target["type"] != "EXECUTABLE":
                continue
            seen = set()
            pending = [target["id"]]
            while pending:
                identity = pending.pop()
                if identity in seen:
                    continue
                seen.add(identity)
                pending.extend(dep["id"] for dep in by_id[identity].get("dependencies", []))
            names = {by_id[identity]["name"] for identity in seen}
            forbidden = names & (TOOLS | {AGGREGATE})
            forbidden |= {other for other in names if other != name
                          and other.startswith("pf-smoke-") and by_name[other]["type"] == "EXECUTABLE"}
            if forbidden:
                raise RuntimeError(f"{config}: {name} depends on {sorted(forbidden)}")
            boundaries[name] = sorted(names)
        (logs / f"{config}-boundaries.json").write_text(json.dumps({
            "compatibility_sources": sources, "compatibility_libraries": sorted(libraries),
            "direct_module_dependencies": boundaries}, indent=2) + "\n")
        run(["cmake", "--build", str(build), "--config", config, "--target",
             AGGREGATE, "pf-compatibility-probe", "pf-smoke-harness-probe",
             "--parallel", str(args.parallel)],
            logs / f"{config}-build.log")
        run(["ctest", "--test-dir", str(build), "-C", config, "-R",
             "^(headless-compatibility-contract|smoke-ownership-audit|smoke-harness-contract)$",
             "--no-tests=error", "--output-on-failure"], logs / f"{config}-tests.log")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
