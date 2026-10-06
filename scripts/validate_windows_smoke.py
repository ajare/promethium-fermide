#!/usr/bin/env python3
"""Build the modular smoke matrix with an explicitly selected MSVC configuration.

Run on Windows: python scripts/validate_windows_smoke.py --build-root out/msvc-smoke
Logs and a CMake File API product inventory are retained under the build root.
This is build validation only; runtime validation belongs to separate tickets.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import validation_run as validation

TOOLS = {
    "pf-restoration-benchmark", "pf-generate-routing-world", "pf-metrics-server",
    "pf-lift-repro", "pf-pause-position-repro", "promethium-fermide-headless",
    "pf-compile-contracts", "pf-simulation-step-timing-checks",
    "pf-occupant-packing-checks", "pf-world-render-slot-checks",
    "pf-sector-tileset-checks", "pf-generate-world", "pf-compatibility-probe",
}


def run(command, log):
    print(subprocess.list2cmdline(command), flush=True)
    with log.open("w", encoding="utf-8") as stream:
        result = validation.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=Path("out/msvc-smoke"))
    parser.add_argument("--build-tree", type=Path,
                        help="reuse one specific tree (requires --gui on or --gui off)")
    parser.add_argument("--generator", default="Visual Studio 18 2026")
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--gui", choices=("on", "off", "both"), default="both")
    parser.add_argument("--high-analysis", action="store_true")
    validation.arguments(parser, default=14400)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("this validation requires Windows and MSVC")
    if args.build_tree and args.gui == "both":
        parser.error("--build-tree requires --gui on or --gui off")
    source = Path(__file__).resolve().parents[1]
    root = args.build_root.resolve()
    modes = ("on", "off") if args.gui == "both" else (args.gui,)
    trees = [args.build_tree.resolve() if args.build_tree else root / ('gui' if mode == 'on' else 'nogui') for mode in modes]
    status = validation.supervise(args, trees)
    if status is not None:
        return status
    for mode in modes:
        build = args.build_tree.resolve() if args.build_tree else root / ("gui" if mode == "on" else "nogui")
        logs = build / "validation-logs"
        logs.mkdir(parents=True, exist_ok=True)
        query = build / ".cmake/api/v1/query/codemodel-v2"
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        run(["cmake", "-S", str(source), "-B", str(build), "-G", args.generator,
             "-A", "x64", f"-DPF_BUILD_GUI={mode.upper()}", "-DBUILD_TESTING=ON",
             f"-DPF_HIGH_ANALYSIS={'ON' if args.high_analysis else 'OFF'}"],
            logs / "configure.log")
        reply = build / ".cmake/api/v1/reply"
        index = json.loads(max(reply.glob("index-*.json")).read_text())
        model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
        configs = {config["name"]: config for config in model["configurations"]}
        if set(configs) != {"Debug", "Release"}:
            raise RuntimeError(f"unexpected configurations: {list(configs)}")
        # A build can regenerate CMake and prune the previous File API reply.
        # Read both configurations before invoking any build commands.
        snapshots = {
            config: [json.loads((reply / ref["jsonFile"]).read_text())
                     for ref in configs[config]["targets"]]
            for config in ("Debug", "Release")
        }
        products = {}
        for config in ("Debug", "Release"):
            targets = {}
            all_targets = {target["id"]: target for target in snapshots[config]}
            for target in snapshots[config]:
                name = target["name"]
                if name in TOOLS or (name.startswith("pf-smoke-") and target["type"] == "EXECUTABLE"):
                    targets[name] = target
            # Inspect the actual generated compiler commands, including source-built
            # dependencies. DLL CRT mismatches must not be masked at link time.
            expected_runtime = "MDd" if config == "Debug" else "MD"
            for target in all_targets.values():
                for group in target.get("compileGroups", []):
                    if group["language"] not in {"C", "CXX"}:
                        continue
                    flags = " ".join(item["fragment"] for item in group.get("compileCommandFragments", []))
                    runtimes = {flag.lstrip("/-") for flag in flags.split()
                                if flag.lstrip("/-") in {"MD", "MDd", "MT", "MTd"}}
                    if runtimes != {expected_runtime}:
                        raise RuntimeError(f"{config} {target['name']}: unexpected CRT {runtimes}")
            missing = TOOLS - targets.keys()
            if missing:
                raise RuntimeError(f"missing targets: {sorted(missing)}")
            if ("pf-smoke-startup" in targets) != (mode == "on"):
                raise RuntimeError("Startup availability does not match GUI capability")
            boundaries = {}
            for name in ("pf-smoke-agent", "pf-smoke-render", "pf-smoke-editor"):
                seen = set()
                pending = [targets[name]["id"]]
                while pending:
                    identity = pending.pop()
                    if identity in seen:
                        continue
                    seen.add(identity)
                    pending.extend(dep["id"] for dep in all_targets[identity].get("dependencies", []))
                closure = [all_targets[identity] for identity in sorted(seen)]
                if any(item["name"] != name and item["name"].startswith("pf-smoke-")
                       and item["type"] == "EXECUTABLE" for item in closure):
                    raise RuntimeError(f"{name} depends on another smoke executable")
                boundaries[name] = {item["name"]: [s["path"] for s in item.get("sources", [])
                                                if "compileGroupIndex" in s] for item in closure}
            (logs / f"{config}-boundaries.json").write_text(json.dumps(boundaries, indent=2) + "\n")
            # First builds prove representative modules do not need the other modules.
            order = ["pf-smoke-agent", "pf-smoke-render", "pf-smoke-editor"]
            order += sorted(targets.keys() - set(order) - {"promethium-fermide-headless"})
            order += ["promethium-fermide-headless"]
            products[config] = {}
            for name in order:
                run(["cmake", "--build", str(build), "--config", config,
                     "--target", name, "--parallel", str(args.parallel)],
                    logs / f"{config}-{name}.log")
                artifacts = [str(build / item["path"]) for item in targets[name].get("artifacts", [])]
                # File API may advertise an optional PDB even when Release has
                # no debug information. Require the primary product, not that PDB.
                primary = [path for path in artifacts
                           if Path(path).name == targets[name].get("nameOnDisk")]
                if any(not Path(path).exists() for path in primary):
                    raise RuntimeError(f"missing {config} product for {name}: {primary}")
                artifacts = [path for path in artifacts if Path(path).exists()]
                if targets[name]["type"] == "EXECUTABLE":
                    expected = build / "bin" / "x64" / config
                    if any(Path(path).parent != expected for path in artifacts):
                        raise RuntimeError(f"unexpected {config} executable location: {artifacts}")
                products[config][name] = artifacts
        (logs / "products.json").write_text(json.dumps(products, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
