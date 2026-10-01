"""Dispatch contracts use synthetic children, never duplicate smoke coverage."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

legacy, probe, *modules = sys.argv[1:]
selections = {
    "--viewport-checks": ["pf-smoke-render"],
    "--render-checks": ["pf-smoke-render"],
    "--agent-behaviour-checks": ["pf-smoke-behaviours", "pf-smoke-editor"],
    "--access-permission-checks": ["pf-smoke-permissions", "pf-smoke-editor"],
    "--route-planning-checks": ["pf-smoke-routing", "pf-smoke-editor", "pf-smoke-render"],
    "--route-planning-time-checks": ["pf-smoke-routing", "pf-smoke-editor", "pf-smoke-render"],
    "--routing-scale-checks": ["pf-smoke-routing"],
    "--shuttle-route-checks": ["pf-smoke-routing"],
    "--restored-path-checks": ["pf-smoke-routing"],
    "--serialization-checks": ["pf-smoke-persistence", "pf-smoke-render"],
    "--coordinated-document-checks": ["pf-smoke-editor"],
    "--metrics-checks": ["pf-smoke-metrics"],
    "--world-teardown-smoke": ["pf-smoke-simulation"],
    "--graphics-startup-smoke": ["pf-smoke-startup"],
}
tools = {
    "--restoration-benchmark": ("pf-restoration-benchmark", []),
    "--write-routing-scale-world": ("pf-generate-routing-world", []),
    "--pause-position-repro": ("pf-pause-position-repro", []),
    "--lift-crossing-repro": ("pf-lift-repro", ["crossing"]),
    "--lift-stall-repro": ("pf-lift-repro", ["boarding"]),
}
suffix = pathlib.Path(legacy).suffix
with tempfile.TemporaryDirectory(prefix="pf compatibility space ") as temporary:
    root = pathlib.Path(temporary)
    executable = root / pathlib.Path(legacy).name
    shutil.copy2(legacy, executable)
    children = set(modules) | {"pf-smoke-startup", "pf-metrics-server"}
    children.update(name for name, _ in tools.values())
    for name in children:
        shutil.copy2(probe, root / (name + suffix))

    def run(args, code=0, mode="", exe=executable, env_extra=None):
        env = dict(os.environ, PF_COMPATIBILITY_PROBE=mode)
        env.update(env_extra or {})
        result = subprocess.run([str(exe), *args], cwd=root.parent, env=env,
                                capture_output=True, text=True, timeout=15)
        assert result.returncode == code, (args, result)
        return result

    def dispatched(result, expected):
        actual = [line[4:] for line in result.stdout.splitlines() if line.startswith("RUN ")]
        assert actual == expected, (actual, expected)
        assert "DEPRECATED:" in result.stderr
        for child in expected:
            assert child in result.stderr
        assert result.stdout.count("CHILD stdout") == len(expected)
        assert result.stderr.count("CHILD stderr") == len(expected)

    # Sequential output is byte-stable, including child forwarding and boundaries.
    first = run([])
    assert first.stdout == run([]).stdout
    dispatched(first, sorted(modules))
    assert modules == sorted(modules)
    lines = first.stdout.splitlines()
    for index, name in enumerate(modules):
        assert lines[index * 3:index * 3 + 3] == ["RUN " + name, "CHILD stdout", "PASS " + name]
    # Resolve siblings from the executable, never CWD or argv[0]/PATH.
    dispatched(run([], exe=executable.name,
                   env_extra={"PATH": str(root) + os.pathsep + os.environ.get("PATH", "")}), modules)
    for option, expected in selections.items():
        dispatched(run([option]), expected)
        bad = run([option, "extra"], 2)
        assert "RUN " not in bad.stdout
    for option, (name, prefix) in tools.items():
        args = ['a path with spaces;$(not-a-shell)"quote', "", "trailing\\", "5"]
        result = run([option, *args])
        dispatched(result, [name])
        assert [line[4:] for line in result.stdout.splitlines() if line.startswith("ARG ")] == prefix + args
    for start in (["--metrics"], ["--metrics-port", "0"],
                  ["--metrics-world", "space world"], ["--metrics-detail=sector,queue"]):
        result = run([*start, "--ticks", "1"])
        dispatched(result, ["pf-metrics-server"])
        expected = {"--metrics": [], "--metrics-port": ["--port", "0"],
                    "--metrics-world": ["--world", "space world"],
                    "--metrics-detail=sector,queue": ["--detail=sector,queue"]}[start[0]]
        assert [line[4:] for line in result.stdout.splitlines() if line.startswith("ARG ")] == expected + ["--ticks", "1"]
    assert "Usage:" in run(["--help"]).stdout
    for args in (["--unknown"], ["--help", "extra"], ["--metrics-port"]):
        assert "ERROR compatibility:" in run(args, 2).stderr
    failed = run(["--render-checks"], 23, "failure")
    assert "FAIL pf-smoke-render: exit 23" in failed.stderr
    failed_all = run([], 23, "failure")
    dispatched(failed_all, modules)  # Failures must not omit later modules.
    abnormal = run(["--render-checks"], 1 if os.name == "nt" else 137, "abnormal")
    assert "abnormal termination" in abnormal.stderr
    child = root / ("pf-smoke-render" + suffix)
    child.unlink()
    missing = run(["--render-checks"], 127)
    assert "missing executable" in missing.stderr
    if os.name != "nt":
        child.write_text("not executable", encoding="utf-8")
        assert "launch failed" in run(["--render-checks"], 126).stderr
print("PASS compatibility dispatch contract")
