"""Headless executable contracts; every subprocess and HTTP request is bounded."""
import os
import pathlib
import signal
import shutil
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

lightweight = sys.argv[-1:] == ['--cli']
benchmark, generator, service, lift, pause, lift_fixture = sys.argv[1:-1] if lightweight else sys.argv[1:]


def run(exe, args, code=0, diagnostic=None, timeout=60):
    result = subprocess.run([exe, *map(str, args)], capture_output=True, text=True, timeout=timeout)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    if code == 0:
        assert not result.stderr, (args, result.stderr)
    if diagnostic:
        assert diagnostic in result.stderr, (args, result.stderr)
    if code == 2:
        assert not result.stdout, (args, result.stdout)
    return result.stdout


for exe in (benchmark, generator, service, lift, pause):
    assert "Usage:" in run(exe, ["--help"])
    for args in (["--unknown"], ["--help", "extra"]):
        run(exe, args, 2, "Usage:")
for exe in (benchmark, generator, lift, pause):
    for args in ([], [""], ["a", "b", "c"]):
        run(exe, args, 2, "Usage:")
for args in (["unknown", lift_fixture], ["crossing"], ["boarding"],
             ["crossing", ""], ["boarding", ""]):
    run(lift, args, 2, "Usage:")
for value in ("0", "1001", "-1", "+1", "1x", "1.0", " 1", "", "999999999999999999999"):
    run(benchmark, ["absent.world.yaml", value], 2, "Cycles")
for option in ("--port", "--ticks", "--world"):
    run(service, [option], 2, "Missing value")
    run(service, [option, ""], 2, "Usage:")
for value in ("-1", "65536", "1x", "+1", "1.0", " 1", "99999999999999999999"):
    run(service, ["--port", value], 2, "Port")
for value in ("0", "1000001", "-1", "1x"):
    run(service, ["--ticks", value], 2, "Ticks")
for args in (["--port", "0", "--port", "0"], ["--ticks", "1", "--ticks", "1"],
             ["--world", "x", "--world", "x"], ["--detail=sector,queue"] * 2,
             ["--detail=sector"], ["--metrics"]):
    run(service, args, 2, "Usage:")

def functional(root, fixture, fixture_bytes, registry, registry_bytes, malformed):
    assert "PASS: Lift boarding stays" in run(lift, ["crossing", fixture])
    assert "PASS: Lift demand drained" in run(lift, ["boarding", fixture])
    assert "PASS: minimal pause-position" in run(pause, ["minimal"])
    # MSVC Debug needs longer for the unchanged file-backed simulation workload.
    assert "PASS: file-backed pause-position" in run(pause, [fixture], timeout=240)
    assert fixture.read_bytes() == fixture_bytes
    assert registry.read_bytes() == registry_bytes
    assert malformed.read_text(encoding="utf-8") == "[invalid: World"

    first, second = root / "first output", root / "second output"
    first.mkdir()
    second.mkdir()
    world = first / "routing scale with spaces.world.yaml"
    output = run(generator, [world])
    assert "PASS: wrote routing stress World and adjacent tag registry" in output
    assert output.count("routing-population agents=1000 vertices=2040") == 2, output
    digests = re.findall(r"digest=(\d+)", output)
    assert len(digests) == 2 and digests[0] == digests[1], output
    assert "PASS:" in run(generator, [second / world.name])
    originals = {p.name: p.read_bytes() for p in first.iterdir()}
    assert len(originals) == 2, originals.keys()
    def normalize(files):
        # New registries intentionally receive fresh UUIDs and tag palette colours.
        result = {}
        for name, data in files.items():
            data = re.sub(rb"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", b"UUID", data)
            if name.endswith(".tags.yaml"):
                data = re.sub(rb"displayColour:\n(?: +[rgb]: \d+\n){3}", b"displayColour: RANDOM\n", data)
            result[name] = data
        return result
    assert normalize(originals) == normalize({p.name: p.read_bytes() for p in second.iterdir()})
    run(generator, [world], 1, "pf-generate-routing-world:")
    assert originals == {p.name: p.read_bytes() for p in first.iterdir()}
    # An existing adjacent registry alone must also prevent overwrite.
    world.unlink()
    run(generator, [world], 1, "pf-generate-routing-world:")
    assert not world.exists()
    world.write_bytes(originals[world.name])
    for args, cycles in (([world, "2"], 2), ([world], 5)):
        report = run(benchmark, args)
        assert report.count("restoration-cycle=") == cycles, report
        for field in ("reload-ms=", "reset-ms=", "working-set-MiB=", "peak-working-set-MiB="):
            assert field in report, report
    assert originals == {p.name: p.read_bytes() for p in first.iterdir()}
    assert "Metrics: http://127.0.0.1:" in run(service, ["--world", world, "--port", "0", "--ticks", "1"])


with tempfile.TemporaryDirectory(prefix="pf tools with spaces ") as temporary:
    root = pathlib.Path(temporary)
    # Pass argv arrays, never pre-quote paths. Exercise both directory and basename
    # spaces on every file-backed tool, including the adjacent registry reference.
    fixture = root / "Lift fixture with spaces.world.yaml"
    shutil.copyfile(lift_fixture, fixture)
    # The checked-in fixture names the bundled TestAgentTags Resource. This
    # standalone tool has no application manifest, so point its reference at the
    # adjacent registry basename the tool can resolve beside the World.
    fixture.write_bytes(fixture.read_bytes().replace(
        b"resource: TestAgentTags", b"resource: test.tags.yaml"))
    fixture_bytes = fixture.read_bytes()
    # The checked-in Lift fixture refers to this adjacent registry by basename.
    registry = root / "test.tags.yaml"
    shutil.copyfile(pathlib.Path(lift_fixture).parent / registry.name, registry)
    registry_bytes = registry.read_bytes()
    missing = root / "missing World.world.yaml"
    run(benchmark, [missing], 1, "pf-restoration-benchmark:")
    run(service, ["--world", missing, "--ticks", "1"], 1, "pf-metrics-server:")
    run(lift, ["crossing", missing], 1, "pf-lift-repro:")
    run(lift, ["boarding", missing], 1, "pf-lift-repro:")
    run(pause, [missing], 1, "pf-pause-position-repro:")
    malformed = root / "malformed World.world.yaml"
    malformed.write_text("[invalid: World", encoding="utf-8")
    run(benchmark, [malformed], 1, "pf-restoration-benchmark:")
    run(service, ["--world", malformed], 1, "pf-metrics-server:")
    run(lift, ["crossing", malformed], 1, "pf-lift-repro:")
    run(lift, ["boarding", malformed], 1, "pf-lift-repro:")
    run(pause, [malformed], 1, "pf-pause-position-repro:")
    run(generator, [root / "absent" / "routing.world.yaml"], 1, "pf-generate-routing-world:")

    if not lightweight:
        functional(root, fixture, fixture_bytes, registry, registry_bytes, malformed)

    # File-backed stdout avoids an unbounded readline waiting for server startup.
    log = root / "service.log"
    with log.open("w") as stream:
        process = subprocess.Popen([service, "--port", "0", "--detail=sector,queue", "--ticks", "300"],
                                   stdout=stream, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            match = None
            while time.monotonic() < deadline and process.poll() is None:
                match = re.search(r"http://127.0.0.1:(\d+)", log.read_text())
                if match:
                    break
                time.sleep(0.02)
            assert match, log.read_text()
            port = match[1]
            base = "http://127.0.0.1:" + port
            # Ignore proxy environment variables for loopback-only tests.
            http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with http.open(base + "/healthz", timeout=3) as response:
                assert response.status == 200 and response.read() == b"ok\n"
            with http.open(base + "/metrics", timeout=3) as response:
                assert "text/plain" in response.headers["Content-Type"]
                assert b"pf_" in response.read()
            for route, method, status in (("/absent", "GET", 404), ("/metrics", "POST", 405)):
                try:
                    http.open(urllib.request.Request(base + route, method=method), timeout=3)
                    raise AssertionError((route, method, "unexpected success"))
                except urllib.error.HTTPError as response:
                    assert response.code == status, (route, response.code)
                    response.close()
            run(service, ["--port", port], 1, "cannot bind", timeout=10)
            _, error = process.communicate(timeout=15)
            assert process.returncode == 0 and not error, error
            # Shutdown must release the listening socket; this also catches a
            # worker surviving the bounded service loop under Windows semantics.
            run(service, ["--port", port, "--ticks", "1"])
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
    if os.name != "nt":
        for stop_signal in (signal.SIGINT, signal.SIGTERM):
            with log.open("w") as stream:
                process = subprocess.Popen([service, "--port", "0"], stdout=stream,
                                           stderr=subprocess.PIPE, text=True)
                try:
                    deadline = time.monotonic() + 10
                    while "Metrics:" not in log.read_text() and time.monotonic() < deadline:
                        assert process.poll() is None
                        time.sleep(0.02)
                    assert "Metrics:" in log.read_text()
                    process.send_signal(stop_signal)
                    _, error = process.communicate(timeout=5)
                    assert process.returncode == 0, error
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate(timeout=5)
print("PASS: standalone tool CLI, persistence, HTTP and shutdown contracts")
