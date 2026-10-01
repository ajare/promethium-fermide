# Independent smoke modules

The first module (#280) owns Simulation Observation. Other domain checks and
legacy commands are unchanged; see the [ownership manifest](smoke-migration-manifest.md).
The legacy aggregate no longer runs Observation, so use CTest for combined coverage.

```sh
cmake -S . -B build-linux -DBUILD_TESTING=ON -DPF_BUILD_GUI=OFF
cmake --build build-linux --target pf-smoke-simulation pf-smoke-harness-probe --parallel
build-linux/bin/x64/Release/pf-smoke-simulation
build-linux/bin/x64/Release/pf-smoke-simulation --list
build-linux/bin/x64/Release/pf-smoke-simulation --check observation
ctest --test-dir build-linux -R 'smoke-(simulation|harness-contract)$' --output-on-failure
```

Use `Debug` in the path for Debug builds; on Windows append `.exe`. Windows
runtime validation remains tracked separately in #279. The runners suppress
Windows error/CRT assertion dialogs and never create a GUI context or window.

## Contract

- No arguments executes the explicit module-local registry in order.
- `--list` prints one stable check name per line without executing checks.
- `--check <name>` executes only that check. No other argument combinations are
  accepted. Misuse prints `ERROR <module>: <diagnostic>` to stderr and returns 2.
- Execution writes one `PASS <module> <check>`, `FAIL <module> <check>: <diagnostic>`,
  or `SKIP <module> <check>: <reason>` record per check to stdout, followed by
  `SUMMARY <module> pass=N fail=N skip=N`. Diagnostics are flattened to one line.
- Ordinary exceptions (including non-standard exceptions) fail the registered
  check but do not prevent later checks from running. Assertions inside a check
  remain fail-fast. Any failure returns 1; otherwise execution returns 0.
- Only `OptionalCapabilityUnavailable` for an unavailable optional external
  capability may skip. Missing required fixtures/libraries/build products fail.
- Context creation failure reports `FAIL <module> setup: ...`, a failed summary,
  and exit 1. Listing and misuse do not create a context or print a summary.

## Narrow support and ownership

`src/headless/smoke/support` supplies failure handling, CLI/reporting, process
setup, required fixture lookup, and an RAII temporary root. It has no production,
ImGui, editor, renderer, or HTTP dependencies. Checks use `Context::fixture()`
with a repository-relative file path; the root is provided by CMake, never the
working directory. They write only beneath `Context::temporaryRoot()`. Each
invocation atomically creates its own randomly named directory beneath the OS
temporary directory, even when concurrent processes run the same check. Cleanup
is best-effort on scope exit, on both pass and failure; crashes/forced termination
can leave directories behind. Failed artifacts are not intentionally retained.
Simulation Observation builds its Worlds in memory and requires no disk fixtures.
The harness probe exercises fixture lookup and temporary file cleanup.

`pf_add_smoke_module` takes explicit `SOURCES`, `LIBRARIES`, `NAME`, `LABELS`, and
`TIMEOUT`. It rejects source ownership shared with another module or the legacy
executable and rejects executable dependencies. It applies ordinary and elevated
analysis warnings, links the narrow support, and registers one direct CTest entry.
All new module/support/test targets exist only with `BUILD_TESTING=ON`; they are
in the default build. Existing standalone and legacy target policies are unchanged.

Simulation links only the production core, its YAML/Lua dependencies, and smoke
support. Its only check translation unit is `simulation/Observation.cpp`; building
`pf-smoke-simulation` never builds the legacy checks or synthetic harness probe.
CTest `smoke-simulation` has `smoke;core` labels, is parallel-safe, and has a
30-second timeout (observed Release runtime about 0.02 seconds). The synthetic
harness contract test has `harness;core` labels so it does not duplicate domain
smoke coverage. Both use the same warning and high-analysis policy.

Domain-specific World builders stay with their module. Do not add editor/render
helpers to core support or make modules depend on other modules' check sources.

## #280 Linux validation

Validated with GCC 15:

- Release, GUI enabled: full default build; all 70 CTest entries pass both
  sequentially and with `-j 6`.
- Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: all project targets build; all 24
  project CTest entries pass sequentially and with `-j 6` (select with
  `ctest --test-dir <build> -E '^willpower_' --output-on-failure`). The unrelated
  vendored Willpower all-target Debug build cannot include `vld.h` on Linux;
  its Debug tests are not claimed as validated.
- Public CLI contract passes in both configurations. Another 64 concurrent
  probe invocations from an external temporary directory produce distinct
  roots and clean them up after both success and failure.
- The Simulation link command contains only smoke support, production core,
  YAML and Lua archives, plus system libraries. A touched Observation check
  rebuild compiles only `simulation/Observation.cpp` (2.51 seconds locally);
  the no-op build compiles nothing. Release execution is about 0.02 seconds.
- CMake helper configuration accepts a valid module and rejects duplicate
  module ownership, legacy source ownership, and executable dependencies.
  `BUILD_TESTING=OFF` exposes no `pf-smoke-*` targets.
- The manifest covers every headless check source. Observation's original
  assertions are unchanged; only its local failure helper was replaced.
  Whitespace validation uses `git diff --check` (no formatter is configured).

Validation also required spelling the default `MobilityProfile` value explicitly
inside its `optional` initializer in `AgentTagAssignmentPanel.cpp`: GCC rejected
the old ambiguous empty-brace initializer. This does not change panel behavior.
