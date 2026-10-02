#!/usr/bin/env python3
"""Validate direct smoke commands from an external, space-containing directory.

Uses CTest's configured target paths, not cwd-based executable discovery. Run after
building the modules: python scripts/validate_smoke_runtime.py --build-tree <tree>
Logs are retained in <tree>/runtime-validation/. All children have timeouts.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-tree", type=Path, required=True)
    parser.add_argument("--configuration", choices=("Debug", "Release"), action="append")
    parser.add_argument("--module", action="append", help="optional smoke module name filter")
    args = parser.parse_args()
    build = args.build_tree.resolve()
    logs = build / "runtime-validation"
    logs.mkdir(exist_ok=True)
    for config in args.configuration or ("Debug", "Release"):
        result = subprocess.run(["ctest", "--test-dir", str(build), "-C", config,
                                 "--show-only=json-v1"], capture_output=True, text=True,
                                check=True, timeout=30)
        tests = json.loads(result.stdout)["tests"]
        modules = {test["name"][6:]: Path(test["command"][0]) for test in tests
                   if test["name"].startswith("smoke-") and test.get("command")
                   and Path(test["command"][0]).name.startswith("pf-smoke-")}
        if args.module:
            missing = set(args.module) - modules.keys()
            if missing:
                raise RuntimeError(f"modules not configured: {sorted(missing)}")
            modules = {name: path for name, path in modules.items() if name in args.module}
        if not modules:
            raise RuntimeError("no smoke modules configured")
        with tempfile.TemporaryDirectory(prefix="pf smoke runtime ") as directory:
            work = Path(directory)
            temp = work / "temporary roots with spaces"
            temp.mkdir()
            env = dict(os.environ, TEMP=str(temp), TMP=str(temp), TMPDIR=str(temp))
            env.pop("DISPLAY", None)
            env.pop("WAYLAND_DISPLAY", None)

            def invoke(name, executable, arguments, status, label):
                if not executable.is_file():
                    raise RuntimeError(f"missing required product: {executable}")
                result = subprocess.run([str(executable), *arguments], cwd=work, env=env,
                                        capture_output=True, text=True, timeout=300)
                (logs / f"{config}-{name}-{label}.log").write_text(
                    result.stdout + result.stderr, encoding="utf-8")
                if result.returncode != status:
                    raise RuntimeError(f"{config} {name} {label}: exit {result.returncode}, expected {status}")
                if status == 2:
                    if result.stdout or not re.fullmatch(rf"ERROR {re.escape(name)}: [^\n]+\n", result.stderr):
                        raise RuntimeError(f"{name}: unstable ERROR record")
                elif result.stderr:
                    raise RuntimeError(f"{name}: unexpected stderr: {result.stderr}")
                return result.stdout

            def execution(name, output, names):
                lines = output.splitlines()
                if len(lines) != len(names) + 1:
                    raise RuntimeError(f"{name}: unexpected records: {output}")
                passed = skipped = 0
                for check, line in zip(names, lines):
                    if line == f"PASS {name} {check}":
                        passed += 1
                    elif line.startswith(f"SKIP {name} {check}: ") and line.split(": ", 1)[1]:
                        # Only the platform-unavailable POSIX semantics are optional
                        # in the configured modules. Missing products/fixtures fail.
                        if name != "persistence" or check not in {
                            "transactional-symlink-target", "transactional-symlink-temp",
                            "transactional-permissions",
                        }:
                            raise RuntimeError(f"unexpected skip: {line}")
                        skipped += 1
                    else:
                        raise RuntimeError(f"{name}: unstable or failed check: {line}")
                if lines[-1] != f"SUMMARY {name} pass={passed} fail=0 skip={skipped}":
                    raise RuntimeError(f"{name}: unstable summary")

            for name, executable in sorted(modules.items()):
                listing = invoke(name, executable, ["--list"], 0, "list")
                names = listing.splitlines()
                if not names or len(set(names)) != len(names) or listing != "\n".join(names) + "\n":
                    raise RuntimeError(f"{name}: invalid check inventory")
                execution(name, invoke(name, executable, [], 0, "all"), names)
                execution(name, invoke(name, executable, ["--check", names[0]], 0, "selected"), names[:1])
                for index, arguments in enumerate((["--bogus"], ["--check"],
                                                   ["--check", "absent-check"], ["--list", "extra"],
                                                   ["--check", names[0], "extra"], [names[0]])):
                    invoke(name, executable, arguments, 2, f"misuse-{index}")
                if list(temp.iterdir()) or set(work.iterdir()) != {temp}:
                    raise RuntimeError(f"{name}: leaked temporary or working-directory output")
                print(f"{config} {name}: {len(names)} checks, CLI and cleanup PASS", flush=True)

            # Filesystem-heavy modules share the same cwd and OS temporary parent.
            # Observe both live roots as well as requiring complete independent output
            # and removal. This detects overlap/leaks without adding diagnostic records
            # to the modules' stable public output.
            for name in ("agent-tags", "behaviours", "persistence", "editor"):
                if name not in modules:
                    continue
                executable = modules[name]
                names = invoke(name, executable, ["--list"], 0, "concurrent-list").splitlines()
                observed = set()
                with ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [pool.submit(invoke, name, executable, [], 0, f"concurrent-{i}")
                               for i in range(2)]
                    while not all(future.done() for future in futures):
                        observed.update(temp.glob("pf-smoke-*"))
                        time.sleep(0.001)
                    for future in futures:
                        execution(name, future.result(), names)
                if len(observed) != 2 or list(temp.iterdir()) or set(work.iterdir()) != {temp}:
                    raise RuntimeError(f"{name}: concurrent roots not independent/clean: {observed}")
                print(f"{config} {name}: two concurrent roots/output/cleanup PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
