# Independent smoke modules

The independent modules currently own Simulation Observation (#280), Render walls
(#281), Persistence serializer/document formats (#282), core World structure
and Sector checks (#284), and Agent identity, activation, and Agent groups (#285). Other domain checks remain legacy-owned; see the
[ownership manifest](smoke-migration-manifest.md). The legacy aggregate no longer
runs migrated checks, `--render-checks` no longer runs migrated walls or core
Window checks, and `--serialization-checks` runs only the remaining serialization
checks. Use CTest for combined coverage.

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
Windows error/CRT assertion dialogs and never create a graphics window. Render
uses only CPU-side ImGui contexts, without platform or GPU backends.

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
All new smoke module/harness/test targets exist only with `BUILD_TESTING=ON`; they are
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

## Agent module (#285)

The Agent module has two independently buildable/runnable dependency tiers:

```sh
cmake --build build-linux --target pf-smoke-agent pf-smoke-agent-editor --parallel
build-linux/bin/x64/Release/pf-smoke-agent --list
build-linux/bin/x64/Release/pf-smoke-agent --check identity
build-linux/bin/x64/Release/pf-smoke-agent-editor --check clipboardCarriesActivation
ctest --test-dir build-linux -R '^smoke-agent' -j 3 --output-on-failure
```

- `pf-smoke-agent` / `smoke-agent` (`smoke;core`) owns 53 registrations:
  typed Agent identity and handle invalidation, activation, Agent group identity,
  naming, persistence, assignment, counts, deletion, ID allocation, and topology.
  It links only smoke support, production core, YAML, and Lua. Building it does
  not compile any editor, ImGui, renderer, HTTP, legacy, or other module checks.
- `pf-smoke-agent-editor` / `smoke-agent-editor` (`smoke;editor`) owns 49
  registrations for panel labels and interactions, clipboard, and document
  history, including mixed World/panel scenarios. It links the actual production
  `pf-agent-editing` seams and CPU ImGui, not application-global stubs, renderer,
  SDL/OpenGL, native dialogs, or HTTP. Both domain tests have 30-second timeouts.
- The eight mixed legacy files were physically split. Their 101 named checks
  are individual registrations, not eight fail-fast suite wrappers. The inline
  ownership check moved from `SmokeScenario.cpp` as `identity`. None of these
  checks remains compiled or invoked by the legacy aggregate. Helpers remain
  local to their domain translation units, outside the narrow smoke support.

Editor checks reset panel/document-history state before and after each check,
including exceptional exit. ImGui contexts disable ini/log files. The hash-name
click check fixes its in-memory window geometry before capturing cell positions;
otherwise first-frame auto-fit moves the cell between capture and click when no
saved `imgui.ini` exists. Clipboard text capture stays in process, and deletion
confirmation tests render only CPU-side ImGui data and answer programmatically:
no desktop window, native dialog, system clipboard, or user input is needed.

`smoke-agent-contract` verifies exact listings, every individual selection, misuse
exit codes, and no files written in an external empty working directory. The
shared `smoke-harness-contract` covers multi-failure continuation. A direct
fault-injection experiment also replaced the first two registrations in each
Agent runner with throwing checks: both returned 1, reported both failures, and
ran the remaining checks (`agent pass=51 fail=2`, `agent-editor pass=47 fail=2`).
The injections were removed and both ordinary binaries rebuilt and revalidated.

### #285 Linux validation

- Release, GUI enabled: full default build (including the GUI sharing the new
  production library), all 79 CTest entries sequentially, and all 78 non-aggregate
  entries with `-j 6` pass. The aggregate runs separately to avoid the documented
  pre-existing legacy temporary-path collision with serialization checks.
- Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: all project targets build and all
  33 project tests pass (excluding vendored `^willpower_` tests).
- A clean GUI-disabled Release build of `pf-smoke-agent` produces only its own
  runner/check objects and smoke support among headless sources. Its actual link
  command contains only smoke support, core, YAML, and Lua. Editor-tier linkage
  adds only `pf-agent-editing` and CPU ImGui; no other check sources are compiled.
- Mechanical comparison verifies all 101 moved named check bodies are unchanged
  except deterministic ImGui geometry setup; the inline identity body is retained.
  Focused contracts pass in Release and Debug, including all single selections
  from an empty directory. No source fixtures or existing untracked files change.
- `git diff --check` passes; no repository formatter is configured. Existing
  high-analysis diagnostics remain warnings. Windows execution remains #279.

## World structure and Sector module (#284)

```sh
cmake --build build-linux --target pf-smoke-world --parallel
build-linux/bin/x64/Release/pf-smoke-world --list
build-linux/bin/x64/Release/pf-smoke-world --check facades
ctest --test-dir build-linux -R '^smoke-world(-contract)?$' --output-on-failure
```

The World module owns eleven core-only groups for Layer queries, Background type,
placement, paint-flow contracts and cascade deletion, Window layer and Background
relationships, Facades, zero-sized Location refusal, and adjacent-Layer threshold
topology. These groups link only smoke support and production core. Their former
sources and invocations were removed from the legacy aggregate, including the two
core Window groups formerly reached by `--render-checks`.

Editor-dependent Background selection and Facade authoring fragments remain in
the editor tier, while Facade drawing remains in the render tier; neither tier is
linked by `pf-smoke-world`. `smoke-world-contract` runs the complete module and
every named selection from an external empty working directory and verifies that
no working-directory files are created. Both tests are headless and require no
ImGui context, renderer, platform backend, graphics window, or dialog support.

## Render pilot (#281)

```sh
cmake --build build-linux --target pf-smoke-render --parallel
build-linux/bin/x64/Release/pf-smoke-render --list
build-linux/bin/x64/Release/pf-smoke-render --check walls
ctest --test-dir build-linux -R 'smoke-(render|simulation|render-contract)$' -j 3 --output-on-failure
```

`render/walls` preserves the six existing wall-rule and real-renderer geometry
checks. `pf-render` compiles `Render.cpp`, `WorldDrawList.cpp`, `SectorTileset.cpp`,
and `ObjectTileset.cpp` once; `pf-imgui-cpu` compiles the four CPU ImGui sources
once. The editor, legacy executable, Render pilot, and standalone tileset checks
reuse these libraries. SDL/OpenGL backends remain editor-only. These production
libraries remain available independently of `BUILD_TESTING`.

`pf-headless-render-support` supplies compiled application-global stubs and
`headless::ScopedImGuiContext` separately from production rendering and core
smoke support. Its object linkage avoids static-archive ordering problems for
renderer-only callers. The scoped context restores any previous context on
normal or exceptional exit and disables ImGui ini/log files. No windows,
platform backends, dialogs, HTTP code, or editor panels are needed by Render.

CTest invokes `pf-smoke-render` directly as `smoke-render`, labelled
`smoke;render`, with a 30-second timeout (observed execution under 0.02 seconds).
`smoke-render-contract` checks listing, selection, misuse exit codes, and absence
of working-directory output from an external temporary directory. It is labelled
`harness;render`, not duplicate domain smoke coverage. Simulation's library and
source dependencies remain unchanged.

## Persistence pilot (#282)

```sh
cmake --build build-linux --target pf-smoke-persistence --parallel
build-linux/bin/x64/Release/pf-smoke-persistence --list
build-linux/bin/x64/Release/pf-smoke-persistence --check world-document-formats
ctest --test-dir build-linux -R '^(smoke-persistence.*|serialization-checks)$' --output-on-failure
```

`persistence/Formats.cpp` owns the coherent YAML/binary serializer contract and
World document-format group extracted from the oversized legacy serialization
suite: primitive encoding, malformed input, file round trips, transactional
opaque bytes, exact suffix dispatch, and format conversion. The six migrated
checks retain their assertions. A seventh check loads the checked-in
`resources/Office.world.yaml` through `Context::fixture()`; it fails rather than
skips if the fixture is missing. No fixture is modified.

All generated files live beneath the invocation's unique Context root. The
opaque-byte and World-document checks use separate subdirectories so existing
file-count and transaction-cleanup assertions retain their meaning. Context
cleanup also runs after assertion failures. The remaining legacy serialization
checks are intentionally not migrated or made concurrent by this ticket.

Persistence links only smoke support and the production core (plus its YAML/Lua
dependencies), with no editor, renderer, ImGui, or HTTP dependency. CTest invokes
it directly as `smoke-persistence`, labelled `smoke;core`, with a 30-second timeout.
`smoke-persistence-contract` checks listing, every single-check selection, misuse,
fixture resolution from an external empty directory, no working-directory output,
and eight concurrent complete invocations. It is labelled `harness;core`.

## Three-module architecture gate (#283)

Validated on Linux with GCC 15 on 1 October 2026. This is the gate for the
representative Simulation, Render, and Persistence pilots; it does not migrate
any additional checks.

- Three clean Release, GUI-disabled build trees each built one direct target:
  `pf-smoke-simulation` in 83.40 seconds, `pf-smoke-render` in 89.90 seconds,
  and `pf-smoke-persistence` in 82.69 seconds (`--parallel 4`). The resulting
  build trees contained only their own pilot check object: `Observation.cpp.o`,
  `Walls.cpp.o`, or `Formats.cpp.o`, respectively. The other two pilot check
  objects were absent in each tree.
- Generated link commands confirmed the intended tiers. Simulation and
  Persistence contain their runner/check objects and link only smoke support,
  production core, YAML, and Lua. Render contains its runner/check objects and
  headless-render support objects, then links smoke support, production render,
  production core, YAML, Lua, and CPU ImGui. None links the legacy executable,
  editor panels, metrics/HTTP, SDL, OpenGL, or another pilot executable.
- Launching the three clean Release executables simultaneously returned zero
  for all three in 35 ms wall time. The summaries reported `1/0/0` for
  Simulation, `1/0/0` for Render, and `7/0/0` for Persistence
  (pass/fail/skip). A direct concurrent Debug CTest run with `-j 3` also passed
  all three in 0.12 seconds.
- A clean Debug, GUI-disabled build with `PF_HIGH_ANALYSIS=ON` built all three
  targets in 67.43 seconds (`--parallel 4`). Exported commands confirmed the
  elevated flags on `Observation.cpp`, `Walls.cpp`, and `Formats.cpp`; the
  three executables then passed concurrently. The warnings emitted are the
  existing diagnostics that analysis mode is designed to expose, not errors.
- A pre-pilot worktree at `0a72e8a` built the original aggregate after applying
  only the behavior-preserving GCC 15 `MobilityProfile` initializer spelling
  already committed by #280. The original aggregate (including Observation),
  `--render-checks`, and `--serialization-checks` each returned zero. The
  migrated modules also returned zero. Mechanical extraction comparison found
  all original assertions unchanged: 16 of 16 Simulation `require` calls,
  35 of 35 Render calls, and 43 of 43 Persistence calls matched after
  whitespace and helper-qualification normalization. Render's scoped CPU-only
  ImGui context and Persistence's Context-owned paths change lifecycle/isolation,
  not the asserted outcomes.
- `git diff --check` passes; no repository formatter is configured. All runs
  were headless. Render created only a CPU-side ImGui context and no platform
  or graphics backend, and none of the pilots can display a dialog.

The clean-build commands used the following pattern, with a separate build
path and corresponding target for each pilot:

```sh
cmake -S . -B /tmp/pf-283-simulation -DCMAKE_BUILD_TYPE=Release \
  -DPF_BUILD_GUI=OFF -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/pf-283-simulation --target pf-smoke-simulation --parallel 4

cmake -S . -B /tmp/pf-283-high -DCMAKE_BUILD_TYPE=Debug \
  -DPF_BUILD_GUI=OFF -DBUILD_TESTING=ON -DPF_HIGH_ANALYSIS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/pf-283-high \
  --target pf-smoke-simulation pf-smoke-render pf-smoke-persistence --parallel 4
ctest --test-dir /tmp/pf-283-high \
  -R '^smoke-(simulation|render|persistence)$' -j 3 --output-on-failure
```

The elapsed times are observations rather than thresholds. Link evidence came
from each target's generated `link.txt`; compilation isolation came from the
objects actually produced and the clean build logs, not from
`compile_commands.json` (which describes all configured targets).

## #282 Linux validation

Validated with GCC 15:

- Release, GUI enabled: full default build and all 74 CTest tests pass
  sequentially. The 73 tests excluding the legacy aggregate also pass with
  `-j 6`, and `headless-smoke` passes separately.
- The unrestricted parallel run exposed an existing fixed-path collision:
  `headless-smoke` and `serialization-checks` both execute the unchanged legacy
  save-transaction checks under `/tmp/pf-save-transaction-smoke`. The aggregate
  failed opening its temporary file while the selected suite removed that root.
  This is outside the extracted group; no global serialization or unrelated
  legacy migration was added to hide it.
- Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: the changed targets and other pilot
  targets build, all 28 project tests pass sequentially (excluding vendored
  `^willpower_` tests), and the 27 non-aggregate project tests pass with `-j 6`.
- A fresh GUI-disabled Release directory builds `pf-smoke-persistence` alone
  (84.52 seconds locally). The only headless objects are its runner, Formats,
  and Smoke support; no legacy, Render, Simulation-check, editor, or ImGui
  sources compile. Its link command contains only smoke support, core, YAML,
  and Lua libraries. A subsequent no-op build compiles nothing.
- Persistence and its public CLI/concurrency contract pass in all three builds,
  including eight simultaneous invocations from an external working directory.
  Direct Release execution is about 0.01 seconds (observations, not thresholds).
- Mechanical comparison confirms all six moved checks' original assertion
  statements are identical and none retain a legacy definition or runner call.
  `git diff --check` passes; no repository formatter is configured. Windows
  execution remains tracked separately in #279.

## #281 Linux validation

Validated with GCC 15:

- Release, GUI enabled: full default build; all 72 CTest entries pass sequentially
  and with `-j 6`.
- Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: all project targets build; all 26
  project CTest entries pass sequentially and with `-j 6` (excluding vendored
  `^willpower_` tests, as in #280).
- Direct Render and Simulation builds and their parallel CTest execution pass.
  The Render CLI contract passes in both configurations from an external empty
  directory and leaves no ini/log files behind.
- Freshly exported compile commands contain exactly one compilation of each of
  the four production rendering and four CPU ImGui sources. Render's link command
  contains only its two check/runner objects, headless support objects, smoke
  support, render, core, YAML, Lua, and CPU ImGui. Simulation still links only
  smoke support, core, YAML, and Lua.
- A touched `render/Walls.cpp` rebuild compiles only that check and relinks Render
  (2.42 seconds locally). A no-op direct Render build compiles nothing (0.16
  seconds). Render execution is under 0.02 seconds; these are observations,
  not thresholds.
- `BUILD_TESTING=OFF` exposes no `pf-smoke-*` targets. All original wall assertions
  are retained; the only check-body change uses the reusable scoped ImGui context.
- `git diff --check` passes; no repository formatter is configured. Windows
  execution validation remains in #279.

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
