"""Verify a Station smoke process exits promptly after reporting its summary."""
import subprocess
import sys


if len(sys.argv) != 2:
    raise SystemExit("usage: station_exit.py <station-smoke-binary>")

process = subprocess.Popen(
    [sys.argv[1], "--check", "stabilityResetIsolationFast"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
summary = "SUMMARY station pass=1 fail=0 skip=0\n"
for line in process.stdout:
    if line == summary:
        break
else:
    stderr = process.stderr.read()
    raise SystemExit(f"Station smoke process ended without a passing summary: {stderr}")

try:
    status = process.wait(timeout=5)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait()
    raise SystemExit("Station smoke process did not exit within five seconds of SUMMARY")

if status != 0:
    raise SystemExit(f"Station smoke process exited with status {status}: {process.stderr.read()}")
