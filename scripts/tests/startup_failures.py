"""Headless synthetic-child negatives and same-process environment isolation (#311)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

startup, probe = map(lambda value: Path(value).resolve(), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="pf startup failures ") as temporary:
    work = Path(temporary)
    child = work / ("editor probe" + probe.suffix)
    shutil.copy2(probe, child)
    env = dict(os.environ, PF_GUI_EXECUTABLE=str(child),
               PF_STARTUP_INHERITED="value with spaces", SDL_VIDEODRIVER="parent-driver",
               DISPLAY="parent-display", WAYLAND_DISPLAY="parent-wayland")

    def run(command, mode, status, diagnostic):
        result = subprocess.run(command, cwd=work, env=dict(env, PF_STARTUP_PROBE=mode),
                                capture_output=True, text=True, timeout=40)
        assert result.returncode == status, (mode, result.returncode, result.stdout, result.stderr)
        assert diagnostic in result.stdout, (mode, result.stdout, result.stderr)
        assert f"SUMMARY startup pass={1 if status == 0 else 0} fail={status} skip=0" in result.stdout

    for mode, status, diagnostic in (
            ("controlled", 0, "PASS startup graphicsInitializationFailure"),
            ("success", 1, "GUI exited with 0"),
            ("wrong", 1, "GUI exited with unexpected status 23"),
            ("abort", 1, "GUI aborted" if os.name == "nt" else "GUI terminated by signal"),
            ("exception", 1, "GUI crashed with exit code 3221225477" if os.name == "nt"
             else "GUI terminated by signal"),
            ("timeout", 1, "GUI executable did not fail fast")):
        run([str(startup)], mode, status, diagnostic)
    # Exercise the actual check in one process, then inspect the environment:
    # a subprocess-only parent assertion would miss the old _putenv_s leak.
    run([str(probe), "--verify-environment"], "controlled", 0,
        "PASS startup graphicsInitializationFailure")
    for key in ("SDL_VIDEODRIVER", "DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(key, None)
    run([str(probe), "--verify-environment"], "controlled", 0,
        "PASS startup graphicsInitializationFailure")
print("PASS Startup controlled status, failure negatives, inherited mode and environment isolation")
