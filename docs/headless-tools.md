# Standalone headless tools

These tools do not run or link smoke checks. Build each independently, including
with `-DPF_BUILD_GUI=OFF -DBUILD_TESTING=OFF`:

```sh
cmake -S . -B build-tools -DPF_BUILD_GUI=OFF -DBUILD_TESTING=OFF
cmake --build build-tools --target pf-restoration-benchmark
cmake --build build-tools --target pf-generate-routing-world
cmake --build build-tools --target pf-metrics-server
cmake --build build-tools --target pf-lift-repro
cmake --build build-tools --target pf-pause-position-repro
```

Executables are in `build-tools/bin/x64/Release` (or `Debug`), with `.exe` on
Windows. Each accepts `--help` alone, prints usage to stdout, and exits **0**.
All commands are non-interactive, including Windows error handling.

## Common exit contract

- **0**: successful completion (or help); reports go to stdout.
- **2**: malformed arguments; a tool-prefixed diagnostic and usage go to stderr.
- **1**: operational failure (load/save, restoration assertion, or HTTP bind);
  a tool-prefixed diagnostic goes to stderr. No retry prompt or dialog appears.

Numbers are whole decimal integers, with no signs, spaces, suffixes, overflow,
NaN, or fractional values. Missing values, unknown options, duplicate service
options, extra positional arguments, and empty paths are rejected before work.
Prefix paths beginning with `-` with `./`.

## Restoration benchmark

```sh
pf-restoration-benchmark routing-scale.world.yaml [cycles]
```

Cycles default to **5**, with an inclusive range of **1–1,000**. The shared
`pf-restoration-support` workload preserves public load/run/reset/run/release,
authored-state and Path digests, simulation traces, and lifetime verification.
Each cycle prints the existing `restoration-cycle=`, `reload-ms=`, `reset-ms=`,
`working-set-MiB=` and `peak-working-set-MiB=` fields. Timings are informational,
not pass/fail thresholds; peak working set remains unavailable (reported as 0)
on Linux. `PF_RESTORATION_TIMING=1` enables nested production phase timings.
Input documents and their registries are not rewritten.

## Lift reproductions

```sh
pf-lift-repro crossing lift-test-1.world.yaml
pf-lift-repro boarding lift-test-1.world.yaml
```

`crossing` verifies the full fixture and its two-Agent reduction, reporting any
boarding grant outside the Lift landing Door's crossing band. `boarding` verifies
the full fixture and its five-Agent opposing-direction reduction, requiring Lift
demand to drain and each reduced journey to finish. Both preserve the former
`--lift-crossing-repro` and `--lift-stall-repro` workflows without linking a
smoke runner.

## Pause-position reproduction

```sh
pf-pause-position-repro minimal
pf-pause-position-repro document.world.yaml
```

`minimal` runs the existing in-memory walking, Path-clear, Staircase, and
Stairwell diagnostics using the checked-in fixtures. A World path runs the
existing sampled full-document diagnostic, reloading and pausing it every 30
ticks from tick 30 through 1,800. Neither mode modifies its fixtures.

## Routing-scale World generator

```sh
pf-generate-routing-world routing-scale.world.yaml
```

The existing population workload still writes a **1,000-Agent / 2,040-Vertex**
World and its adjacent `.tags.yaml` registry, verifies the document round-trip,
and reports cold/warm routing measurements and Path digests followed by
`PASS: wrote routing stress World and adjacent tag registry`. New registry UUIDs
and tag palette colours remain intentionally variable. Neither existing output
nor existing registry is overwritten; the parent directory must already exist.
This is separate from `pf-generate-world`, the authored starter-World generator.

## Metrics server

```sh
pf-metrics-server [--port 0..65535] [--world document.world.yaml] \
    [--detail=sector,queue] [--ticks 1..1000000]
```

No arguments starts the service on **127.0.0.1:9464** with one empty Room.
`--port 0` requests an ephemeral port; stdout announces the actual URL after
successful binding. `--world` loads an authored World instead. Detail is opt-in.
The server uses shared production `pf-metrics`, not smoke support.

Normally it runs until SIGINT/SIGTERM, then stops the HTTP worker, detaches the
observer, and exits **0**. On Windows, console Ctrl-C delivers SIGINT; forced
process termination does not run cleanup. `--ticks N` provides bounded automation: run N service
iterations (advance one simulation tick, or refresh when paused), sleeping 16 ms
per iteration, then cleanly exit **0**. Bind failure exits **1** immediately,
unlike the GUI's non-fatal endpoint failure. See [metrics](metrics.md) for HTTP
routes, exposition, security, and GUI behavior.

## Migration and validation

The old `promethium-fermide-headless --restoration-benchmark`,
`--write-routing-scale-world`, `--metrics*`, `--lift-crossing-repro`,
`--lift-stall-repro`, and `--pause-position-repro` selections now launch the
corresponding tool as a subprocess with deprecation guidance. See the
[compatibility contract](headless-compatibility.md) for argument translation and
exit handling. No tool runs as part of the no-argument smoke aggregate.

With testing enabled, Python 3 runs the bounded, headless subprocess/HTTP contract:

```sh
ctest --test-dir build-linux -R '^headless-tools-contract$' --output-on-failure
```

It checks valid and malformed CLI calls, Lift and pause-position reproductions,
generation/registry preservation, restoration reports, missing/malformed
documents, HTTP health and metrics, occupied-port failure, and bounded shutdown,
using unique temporary directories and file paths containing spaces. It also
checks stdout/stderr separation, fixture preservation, unsupported HTTP routes
and methods, and immediate port rebinding after shutdown. For explicit MSVC
Debug/Release builds, executable paths containing spaces, and the Windows signal
handling distinction, see [Windows tool validation](windows-tools-validation.md).
The Transports, Simulation, Routing, Persistence, and Metrics smoke modules retain
ownership of their existing behavioral checks.
