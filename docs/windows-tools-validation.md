# Windows dedicated-tool validation (#310)

The five [headless tools](headless-tools.md) remain separate executables. They
compile their own command entry points and shared production/workload support,
not smoke implementations or the compatibility dispatcher.

## Reproduce

From PowerShell, with CMake, Python 3, and a supported x64 MSVC toolchain installed:

```powershell
cmake -S . -B "out\tools with spaces" -G "Visual Studio 18 2026" -A x64 -DPF_BUILD_GUI=OFF -DBUILD_TESTING=ON
python scripts/validate_windows_tools.py --build-tree "out\tools with spaces"
```

Use your installed Visual Studio generator. The validator preserves the existing
build-tree options, builds **each tool directly** with `--config Debug` and
`--config Release`, and runs only `headless-tools-contract` in each configuration.
It also audits the CMake File API for each tool's own entry-point sources and
transitive dependencies: no smoke implementations or other executables are
allowed. Logs and configuration-specific product inventories are written under
`<build-tree>/tool-validation-logs`.

To repeat just the contract after building:

```powershell
ctest --test-dir "out\tools with spaces" -C Debug -R "^headless-tools-contract$" --no-tests=error --output-on-failure
ctest --test-dir "out\tools with spaces" -C Release -R "^headless-tools-contract$" --no-tests=error --output-on-failure
```

Direct invocation uses PowerShell's call operator; quote each argument separately:

```powershell
& "out\tools with spaces\bin\x64\Release\pf-pause-position-repro.exe" minimal
& "out\tools with spaces\bin\x64\Release\pf-metrics-server.exe" --port 0 --ticks 1
```

The contract checks all five help/malformed/operational-failure/success paths,
stdout/stderr routing and 0/1/2 exits. Unique temporary directories and document
basenames contain spaces. The Lift fixture is copied with its adjacent Agent tag
registry; both Lift modes and both pause-position modes retain their diagnostics.
Generated Worlds and adjacent registries reload, existing outputs are refused,
and restoration cycles leave both input files byte-for-byte unchanged.

The loopback service test discovers an ephemeral port, checks health and metrics,
404/405 responses, an occupied-port failure bounded to ten seconds, and clean
bounded shutdown followed by immediate rebinding of the same port. Subprocesses
and HTTP requests have deadlines; no retry input or OS dialog is expected.

## Windows shutdown distinction

Windows console Ctrl-C delivers SIGINT to the service. For unattended Windows
validation, use `--ticks`: it exercises the normal HTTP-worker stop/join and
observer-detachment path without depending on an attached interactive console.
Python's `Popen.send_signal(SIGTERM)` on Windows uses `TerminateProcess`, not a
catchable POSIX signal; it cannot establish graceful shutdown and is intentionally
not used as that test. The POSIX SIGINT/SIGTERM contract remains tested on other
platforms. Forced termination is only a failed-test cleanup fallback.

## Recorded results

Validated on Windows x64 with Visual Studio 18 2026, MSVC **19.51.36256.0**, CMake
**4.4.0**, and Python **3.13**:

| Build tree | GUI | Debug contract | Release contract |
| --- | --- | --- | --- |
| `out/307-gui` | ON | Passed (283.84 s) | Passed (27.64 s) |
| `out/310 tools with spaces` (fresh) | OFF | Passed (256.27 s) | Passed (26.80 s) |

Each of the five targets built directly in both configurations in both trees;
the second tree also validates executable paths containing spaces. Two Release
contracts ran concurrently in the separate trees and passed (26.59/26.76 s),
exercising unique temporary directories and ephemeral ports concurrently under
Windows filesystem/network semantics.

No production command-line, path, filesystem, bind, or bounded-shutdown defect
was found. The changes strengthen the regression contract and add reproducible
Windows validation; public tool behavior and independent ownership are unchanged.
