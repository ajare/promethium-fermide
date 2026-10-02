#!/usr/bin/env python3
"""Build and validate the five dedicated tools directly in MSVC Debug/Release."""
import argparse
import json
from pathlib import Path
import sys

from validate_windows_smoke import run

TOOLS = {
    "pf-restoration-benchmark": {"RestorationBenchmark.cpp", "ProcessMemory.cpp"},
    "pf-generate-routing-world": {"GenerateRoutingWorld.cpp", "ProcessMemory.cpp"},
    "pf-metrics-server": {"MetricsServer.cpp"},
    "pf-lift-repro": {"LiftRepro.cpp"},
    "pf-pause-position-repro": {"PausePositionRepro.cpp"},
}


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
    logs = build / "tool-validation-logs"
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
    # Snapshot both configurations before a build can regenerate the File API.
    snapshots = {config["name"]: [json.loads((reply / ref["jsonFile"]).read_text())
                                  for ref in config["targets"]]
                 for config in model["configurations"]}
    for config, targets in snapshots.items():
        by_name = {target["name"]: target for target in targets}
        by_id = {target["id"]: target for target in targets}
        products = {}
        for name, expected_sources in TOOLS.items():
            target = by_name[name]
            sources = {item["path"] for item in target["sources"] if "compileGroupIndex" in item}
            expected = {"src/headless/tools/" + item for item in expected_sources}
            if sources != expected:
                raise RuntimeError(f"{config}: {name} compiles unexpected sources: {sources}")
            seen = set()
            pending = [target["id"]]
            while pending:
                identity = pending.pop()
                if identity in seen:
                    continue
                seen.add(identity)
                dependency = by_id[identity]
                if dependency["name"].startswith("pf-smoke-") or any(
                    "/smoke/" in item["path"] for item in dependency.get("sources", [])
                    if "compileGroupIndex" in item
                ):
                    raise RuntimeError(f"{config}: {name} links smoke implementations")
                if identity != target["id"] and dependency["type"] == "EXECUTABLE":
                    raise RuntimeError(f"{config}: {name} depends on another executable")
                pending.extend(dep["id"] for dep in dependency.get("dependencies", []))
            products[name] = {
                "sources": sorted(sources),
                "dependencies": sorted(by_id[identity]["name"] for identity in seen),
                "artifacts": [str(build / item["path"]) for item in target["artifacts"]],
            }
            # Build each target separately; no compatibility executable or smoke
            # module is used as a build or execution umbrella.
            run(["cmake", "--build", str(build), "--config", config, "--target", name,
                 "--parallel", str(args.parallel)], logs / f"{config}-{name}-build.log")
        (logs / f"{config}-products.json").write_text(json.dumps(products, indent=2) + "\n")
        run(["ctest", "--test-dir", str(build), "-C", config, "-R",
             "^headless-tools-contract$", "--no-tests=error", "--output-on-failure"],
            logs / f"{config}-contract.log")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
