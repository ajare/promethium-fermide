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
    workspace = pathlib.Path(temporary)
    configuration = pathlib.Path(legacy).parent.name
    if configuration not in ("Debug", "Release"):
        configuration = "Debug"
    root = workspace / "bin" / "x64" / configuration
    other = root.parent / ("Release" if configuration == "Debug" else "Debug")
    cwd = workspace / "unrelated working directory"
    for directory in (root, other, cwd):
        directory.mkdir(parents=True)
    executable = root / pathlib.Path(legacy).name
    shutil.copy2(legacy, executable)
    children = set(modules) | {"pf-smoke-startup", "pf-metrics-server"}
    children.update(name for name, _ in tools.values())
    for name in children:
        shutil.copy2(probe, root / (name + suffix))
        # Deliberately invalid products must never be discovered in another
        # configuration, CWD, or PATH (even when the correct sibling is missing).
        for directory in (other, cwd):
            (directory / (name + suffix)).write_text("wrong product", encoding="utf-8")

    def run(args, code=0, mode="", exe=executable, env_extra=None):
        env = dict(os.environ, PF_COMPATIBILITY_PROBE=mode)
        env.update(env_extra or {})
        env["PATH"] = env.get("PATH", "") + os.pathsep + str(cwd)
        # Windows CreateProcess searches the parent's PATH, not the supplied
        # child environment. Temporarily change it only for bare-name invocation.
        previous_path = os.environ.get("PATH", "")
        if str(exe) == pathlib.Path(exe).name:
            os.environ["PATH"] = env["PATH"]
        previous_mode = None
        if os.name == "nt" and env.get("PF_COMPATIBILITY_ERROR_MODE"):
            import ctypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            previous_mode = kernel.GetErrorMode()
            kernel.SetErrorMode(previous_mode | 0x0004)  # caller's alignment flag
        try:
            result = subprocess.run([str(exe), *args], cwd=cwd, env=env,
                                    capture_output=True, text=True, timeout=15)
        finally:
            os.environ["PATH"] = previous_path
            if previous_mode is not None:
                kernel.SetErrorMode(previous_mode)
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
        args = ['a path with spaces;$(not-a-shell)"quote', "", "trailing\\", "5",
                'embedded\\\\"quote', 'space with trailing\\', '"',
                '& echo injected > shell-sentinel', '%PATH%', '^|<>()!']
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
    # Multiple translated options, empty values, and literal shell syntax.
    metrics = run(["--metrics", "--metrics-port", "0", "--metrics-world", "",
                   "--metrics-detail=sector,queue", "--ticks", "1", "a&b\"c"])
    assert [line[4:] for line in metrics.stdout.splitlines() if line.startswith("ARG ")] == [
        "--port", "0", "--world", "", "--detail=sector,queue", "--ticks", "1", "a&b\"c"]
    inherited = 'inherited space "quotes" & %PATH% \\'
    environment = run(["--render-checks"], env_extra={"PF_COMPATIBILITY_ENVIRONMENT": inherited})
    assert "ENV " + inherited in environment.stdout.splitlines()
    if os.name == "nt":
        environment = run(["--render-checks"], env_extra={"PF_COMPATIBILITY_ERROR_MODE": "1"})
        mode = int(next(line[11:] for line in environment.stdout.splitlines()
                        if line.startswith("ERROR_MODE ")))
        assert mode & 0x8007 == 0x8007, mode  # required flags plus caller flag
        normalized = run(["--render-checks"], 1,
                         env_extra={"PF_COMPATIBILITY_EXIT_pf-smoke-render": "300"})
        assert "Windows exit 300 normalized to 1" in normalized.stderr
    assert not (cwd / "shell-sentinel").exists()
    assert "Usage:" in run(["--help"]).stdout
    for args in (["--unknown"], ["--help", "extra"], ["--metrics-port"]):
        assert "ERROR compatibility:" in run(args, 2).stderr
    failed = run(["--render-checks"], 23, "failure")
    assert "FAIL pf-smoke-render: exit 23" in failed.stderr
    failed_all = run([], 23, "failure")
    dispatched(failed_all, modules)  # Failures must not omit later modules.
    abnormal_code = 1 if os.name == "nt" else 137
    abnormal = run(["--render-checks"], abnormal_code, "abnormal")
    assert "abnormal termination" in abnormal.stderr
    if os.name == "nt":
        assert "Windows status 3221225477" in abnormal.stderr  # 0xC0000005
    abnormal_all = run([], abnormal_code, "abnormal")
    dispatched(abnormal_all, modules)
    assert abnormal_all.stderr.count("abnormal termination") == len(modules)
    # Unlike identical failures, these prove the result belongs to the first
    # failing module rather than the last, while all children still run.
    mixed = run(["--route-planning-checks"], 23, env_extra={
        "PF_COMPATIBILITY_EXIT_pf-smoke-routing": "23",
        "PF_COMPATIBILITY_EXIT_pf-smoke-editor": "42"})
    dispatched(mixed, selections["--route-planning-checks"])
    assert "FAIL pf-smoke-editor: exit 42" in mixed.stderr
    mixed_all = run([], 23, env_extra={
        "PF_COMPATIBILITY_EXIT_" + modules[0]: "23",
        "PF_COMPATIBILITY_EXIT_" + modules[-1]: "42"})
    dispatched(mixed_all, modules)
    child = root / ("pf-smoke-render" + suffix)
    child.unlink()
    missing = run(["--render-checks"], 127)
    assert "missing executable" in missing.stderr
    missing_last = run(["--route-planning-checks"], 23, env_extra={
        "PF_COMPATIBILITY_EXIT_pf-smoke-routing": "23"})
    assert "missing executable" in missing_last.stderr
    assert missing_last.stdout.count("CHILD stdout") == 2
    (root / ("pf-smoke-behaviours" + suffix)).unlink()
    missing_first = run(["--agent-behaviour-checks"], 127, env_extra={
        "PF_COMPATIBILITY_EXIT_pf-smoke-editor": "42"})
    assert "missing executable" in missing_first.stderr
    assert "FAIL pf-smoke-editor: exit 42" in missing_first.stderr
    # An existing invalid image exercises CreateProcess failure, not discovery.
    child.write_text("not executable", encoding="utf-8")
    startup = run(["--render-checks"], 126)
    assert "launch failed" in startup.stderr
    if os.name == "nt":
        # Windows versions may classify a malformed image as BAD_EXE_FORMAT
        # (193) or EXE_MACHINE_TYPE_MISMATCH (216).
        assert any(f"Windows error {code}" in startup.stderr for code in (193, 216)), startup.stderr
    startup_last = run(["--route-planning-checks"], 42, env_extra={
        "PF_COMPATIBILITY_EXIT_pf-smoke-editor": "42"})
    assert "launch failed" in startup_last.stderr
    assert startup_last.stdout.count("CHILD stdout") == 2
    (root / ("pf-smoke-routing" + suffix)).write_text("not executable", encoding="utf-8")
    startup_first = run(["--route-planning-checks"], 126, env_extra={
        "PF_COMPATIBILITY_EXIT_pf-smoke-editor": "42"})
    assert "FAIL pf-smoke-editor: exit 42" in startup_first.stderr
    assert startup_first.stderr.count("launch failed") == 2
print("PASS compatibility dispatch contract")
