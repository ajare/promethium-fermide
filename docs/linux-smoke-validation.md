# Linux modular smoke validation

See [bounded validation and recovery](validation-recovery.md) for run/build budgets,
process ownership, interruption handling, logs, and guarded `--rerun-failed`.
`--lane final all` builds the default inventory; use the unfiltered final CTest
lane afterwards for complete standalone/dependency coverage.

This is the active Linux build and validation procedure for the completed smoke
migration. It is entirely non-interactive: `DISPLAY` and `WAYLAND_DISPLAY` are
unset, Render and Editor checks use CPU-only ImGui, and Startup deliberately uses
an unavailable SDL driver and rejects abnormal child termination. **Windows
validation is not claimed here; it remains delegated to #279.**

## Validation lanes

See [smoke validation lanes](smoke-validation-lanes.md) for explicit fast-development
and unfiltered exhaustive-final commands, artifact-risk selections, scheduler
accounting, and Debug/Release timing evidence. Unfiltered CTest retains all stress
assertions. The incremental helper defaults to `--lane fast`; choose `--lane final`
for selected modules' exhaustive contracts.

## Incremental smoke validation

From the repository root, select a required build configuration and one or more
smoke-test sets:

```sh
scripts/validate_linux_smoke.sh --config Release routing simulation
scripts/validate_linux_smoke.sh --config Debug world
scripts/validate_linux_smoke.sh --config Release all
```

List the command arguments and available test sets without configuring a build:

```sh
scripts/validate_linux_smoke.sh --list
```

The script incrementally configures its existing
`build-linux-validation/<debug|release>` tree, builds only the selected
`pf-smoke-*` targets and the harness probe, and runs their selected lane's
functional/CLI/isolation CTest entries. `--lane final` additionally runs their
exhaustive contracts. Use `--build-dir path`
to select another persistent build tree. GUI support remains enabled so the
Startup set and its editor subprocess are available from the same tree. Displays
are unset for every run.

Parallelism defaults to `nproc` and can be limited with `PF_VALIDATION_JOBS`:

```sh
PF_VALIDATION_JOBS=4 scripts/validate_linux_smoke.sh --config Release all
```

## Focused build helpers

The ordinary Linux helpers now configure testing explicitly and accept elevated
analysis:

```sh
./build_from_scratch.sh --config Release --gui
./build_from_scratch.sh --config Release --no-gui --build-dir build-release-headless
./build_from_scratch.sh --config Debug --gui --build-dir build-debug-gui
./build_from_scratch.sh --config Debug --no-gui --high-analysis \
  --build-dir build-debug-analysis
./build_incremental.sh --config Debug --no-gui --high-analysis \
  --build-dir build-debug-analysis
```

Use direct module commands for active smoke work:

```sh
cmake --build build-linux --target pf-smoke-routing pf-smoke-harness-probe --parallel
build-linux/bin/x64/Release/pf-smoke-routing --list
build-linux/bin/x64/Release/pf-smoke-routing --check populationRouting
ctest --test-dir build-linux -R '^smoke-routing($|-)' -L '^validation-fast$' --output-on-failure
```

The old `prometheum-fermide-headless --...` forms are compatibility examples only.
They emit deprecation guidance and dispatch sibling module/tool executables; they
are not CTest owners. See [headless compatibility](headless-compatibility.md).

## Ownership and dependency audit

`smoke-ownership-audit` is part of every testing-enabled CTest inventory:

```sh
ctest --test-dir build-linux -R '^smoke-ownership-audit$' --output-on-failure
```

It compares the authoritative current table in the
[ownership manifest](smoke-migration-manifest.md) with the explicit module source
lists and every check translation unit. Configuration already rejects a source
owned by two modules; the audit additionally rejects missing, duplicate, stale,
or mismatched manifest rows. Runner, state, support, tools, probes, compatibility,
and compile-only sources are deliberately not counted as smoke execution owners.

## Recorded #306 evidence

This section records the final Linux run for #306. Durations are local observations,
not portable limits.

Validated on Linux 7.0 x86-64 with GCC 15.2 on 2 October 2026:

- Fresh default builds passed for Release/GUI, Release/headless, Debug/GUI, and
  Debug/headless. The last configuration used `PF_HIGH_ANALYSIS=ON`; exported
  commands contain all elevated warning flags. The Linux-only CMake override for
  Willpower's Debug `WP_USE_MEMLEAK_TRACKING` prevents its MSVC-only `vld.h` include
  while leaving the dependency's Debug definitions unchanged on Windows.
- The GUI-enabled Release inventory contained 84 CTests. All passed sequentially
  and at `-j 8`; the optional vendored GUI capability test was the sole explicit
  skip in each run. Displays were unset and Startup's controlled child failure
  completed normally, so no window or dialog was used.
- All 13 configured smoke modules built directly, listed their public inventory,
  ran one selected check, and completed their full registry. Generated link
  commands contained no other module's check objects. Core tiers linked neither
  Render nor Editor; Render linked production rendering only; Editor linked
  production rendering/editing; Metrics alone used its HTTP service boundary.
- All five dedicated tools passed `--help` and bounded real commands. The routing
  generator wrote and reloaded a 1,000-Agent/2,040-Vertex World; its output passed
  one restoration cycle. Metrics bound an ephemeral loopback port for one tick.
  Both Lift modes and the minimal pause-position reproduction passed. The four
  standalone check executables and `pf-compile-contracts` also passed directly.
- The ownership audit accounted for 160 modular check sources and four retained
  standalone check sources exactly once. There are no smoke checks in the
  compatibility executable.
- A fresh direct `pf-smoke-world` build took 59,783 ms. An immediate no-op build
  took 204 ms and compiled nothing. Touching only `world/MarkerIdentity.cpp`
  rebuilt that object and relinked only `pf-smoke-world` in 1,870 ms.
- Complete module runtimes were:

| Module | Runtime (ms) |
| --- | ---: |
| Agent | 117 |
| Agent tags | 42 |
| Behaviours | 219 |
| Editor | 357 |
| Metrics | 9 |
| Permissions | 5,215 |
| Persistence | 435 |
| Render | 41 |
| Routing | 2,591 |
| Simulation | 349 |
| Startup | 105 |
| Transports | 2,811 |
| World | 185 |

`git diff --check`, shell syntax checks, and Python ownership-audit execution
passed. These values are Linux observations only. Windows validation remains
explicitly delegated to #279 and is not part of this evidence.
