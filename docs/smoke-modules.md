# Independent smoke modules

The independent modules currently own Simulation Observation (#280), Render walls
(#281), Persistence serializer/document formats (#282), core World structure
and Sector checks (#284), Agent identity, activation, and Agent groups (#285), and individual Agent
properties (#286), Agent tags and coordinated documents (#287), and Agent
behaviour registry and authoring (#288), Agent behaviour runtime (#289), and
Access permissions and Interaction points (#290), Route planning and movement (#291),
Transit/transport runtime checks (#292), and pathfinding scale and perceived
route costs (#293).
Other domain checks remain legacy-owned; see the
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

## Transports module (#292)

```sh
cmake --build build-linux --target pf-smoke-transports pf-smoke-transports-editor pf-smoke-render --parallel
build-linux/bin/x64/Release/pf-smoke-transports --list
build-linux/bin/x64/Release/pf-smoke-transports --check liftBoardingReduced
ctest --test-dir build-linux -R '^smoke-(transports|render)' -j 3 --output-on-failure
```

- `pf-smoke-transports` / `smoke-transports` owns 41 individually selectable
  core checks: Staircases and Stairwells, Platform lifts, Lifts, Shuttles,
  occupant clearance/order/re-spacing, boarding, Door queries, Escalator walking,
  and onboard Agent deletion. It links only smoke support, boarding assertion
  support, and production core/YAML/Lua. The timeout is 120 seconds.
- `pf-smoke-transports-editor` owns the coherent Escalator property workflow
  that asserts production registry undo/redo. It uses the existing headless
  editing libraries, resets editor state on success/failure, and has a 30-second
  timeout. Core runtime checks have no editor dependency.
- Render owns `carriageDoors` and `carriageImages`, preserving wireframe ordering,
  leaf exclusion, plain shaft, and composable carriage-image assertions. CPU-only
  ImGui disables ini/log files; viewport and tileset state is cleaned on exit.
- Four full/reduced boarding/crossing checks load `lift-test-1.world.yaml` through
  Context, independently of the working directory. The legacy file-driven
  reproduction CLI remains separate, sharing compiled assertions rather than
  duplicating check bodies. Its former two CTest entries are retired.
- Escalator Lua packages live beneath the unique Context temporary root.
  `smoke-transports-contract` checks exact listings, all individual selectors,
  misuse, empty-directory operation, and eight concurrent runs of each tier.
  Transport permission checks remain owned by #290; movement checks by #291;
  perceived route-cost migration remains #293.

### #292 Linux validation

- GUI-enabled Release default build and all 87 CTest entries pass sequentially;
  all 86 excluding the legacy aggregate also pass with `-j 6` (avoiding its
  pre-existing shared temporary-path collision). Both reproduction commands
  continue to work independently of the module CLI.
- A fresh GUI-disabled Release build of only `pf-smoke-transports` passes CTest.
  Its only headless objects are its ten runner/check files, smoke support, and
  boarding support; its link includes no editor, renderer, ImGui, HTTP, or other
  module checks.
- Debug with `PF_HIGH_ANALYSIS=ON`: Transports, Transports Editor, Render, and
  legacy headless targets build; all five focused CTest entries pass, including
  selection and concurrent-invocation contracts. Legacy aggregate, render, and
  serialization tests also pass. The full Debug default build hits the existing
  vendored non-Windows `vld.h` failure; relevant project targets build directly.
- All 26 extracted inline scenario bodies remain byte-identical. All 135 original
  `require` call sites in Escalator, deletion, Door-query, and transport-render
  sources remain intact; boarding helper assertions are unchanged. No production
  simulation code changes. `git diff --check` passes; no formatter is configured.

## Agent module (#285)

The Agent module has two independently buildable/runnable dependency tiers:

```sh
cmake --build build-linux --target pf-smoke-agent pf-smoke-agent-editor --parallel
build-linux/bin/x64/Release/pf-smoke-agent --list
build-linux/bin/x64/Release/pf-smoke-agent --check identity
build-linux/bin/x64/Release/pf-smoke-agent-editor --check clipboardCarriesActivation
ctest --test-dir build-linux -R '^smoke-agent' -j 3 --output-on-failure
```

- `pf-smoke-agent` / `smoke-agent` (`smoke;core`) owns 63 registrations:
  typed Agent identity and handle invalidation, activation, Agent group identity,
  naming, persistence, assignment, counts, deletion, ID allocation, topology,
  Colour, Walk speed, Height, and individual-property precedence/persistence.
  It links only smoke support, production core, YAML, and Lua. Building it does
  not compile any editor, ImGui, renderer, HTTP, legacy, or other module checks.
- `pf-smoke-agent-editor` / `smoke-agent-editor` (`smoke;editor`) owns 57
  registrations for panel labels and interactions, clipboard, document history,
  and the migrated property checks that exercise Selection text or real CPU-side
  rendering. It links the actual production `pf-agent-editing`, Render, and
  CPU-ImGui seams, but no platform backend, SDL/OpenGL, native dialogs, or HTTP.
  Both domain tests have 30-second timeouts.
- The eight #285 mixed legacy files were physically split. Their 101 named checks
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

#286 adds 18 individually selectable property checks. Core registration files
have no editor, ImGui, or renderer includes and `pf-smoke-agent` still links only
smoke support and production core. Checks using tag editor transactions, Selection
panel output, or real CPU-side rendering live in `pf-smoke-agent-editor`. The old
four legacy sources and aggregate calls no longer exist, so each migrated check
has one execution owner.

### #286 Linux validation

- A clean GUI-disabled Release build of `pf-smoke-agent` compiles no editor,
  ImGui, renderer, or migrated editor-check objects; its link command contains
  only smoke support, production core, YAML, and Lua. All 63 checks pass.
- The Release GUI-enabled default build and all 79 CTest entries pass
  sequentially. The 78 tests excluding the legacy aggregate also pass with
  `-j 6`; the focused Agent tests and exact-list/selection contract pass.
- A clean Debug GUI-disabled `PF_HIGH_ANALYSIS=ON` build of both Agent tiers
  succeeds and all three Agent CTest entries pass concurrently. Elevated-analysis
  output contains existing project diagnostics, with no new build failure.
- Mechanical comparison found every original assertion in the four migrated
  sources in the new owners. `git diff --check` passes; no repository formatter
  is configured. The editor checks use only scoped CPU-side ImGui contexts and
  produce no ini/log files, windows, native dialogs, or required user input.

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

## Agent tag modules (#287)

```sh
cmake --build build-linux --target pf-smoke-agent-tags pf-smoke-agent-tags-editor --parallel
build-linux/bin/x64/Release/pf-smoke-agent-tags --list
build-linux/bin/x64/Release/pf-smoke-agent-tags-editor --check oneEditUpdatesAndRestoresTwoWorlds
ctest --test-dir build-linux -R '^smoke-agent-tags' -j 3 --output-on-failure
```

The ten former Agent tag sources now have 51 individual registrations:

- `pf-smoke-agent-tags` / `smoke-agent-tags` (`smoke;core`) owns 13 checks for
  external registry identity, canonical sharing, transactional refusal, assignment,
  persistence, reconciliation, and Mobility profile inheritance. It links only
  smoke support and production core (including YAML/Lua).
- `pf-smoke-agent-tags-editor` / `smoke-agent-tags-editor` (`smoke;editor`) owns
  38 checks for registry editing and selection, assignments, clipboard, deletion,
  detach/switch, reload, coordinated multi-World transactions and undo/redo,
  dependency-ordered saves, Save As, and dirty-document prompt text. Mixed checks
  retain their complete scenarios and assertions in this tier. It reuses the
  production editing and CPU-only rendering support libraries, without native
  dialogs, HTTP, SDL/OpenGL, platform backends, or a graphics window.

Both targets are independently buildable, with direct 30-second CTest entries.
The legacy sources, declarations, and invocations are removed, including tag
coverage formerly reached by `--coordinated-document-checks`. Use CTest for
combined coverage. #288 removes the remaining behaviour portability checks and
retires that legacy selection (exit 2 with directions to the independent modules).

Every filesystem fixture uses a distinct directory below the harness Context's
unique temporary root, including multiple fixtures within one check. Context
cleans the files on success or exception. Editor registrations reset tag-panel
state, pending confirmations, World undo history, and write-failure injection on
entry and exit. The Selection checklist uses scoped CPU ImGui with ini/log files
disabled and in-process clipboard capture. Confirmations are answered directly
through production seams, never through native dialogs or user input.

`smoke-agent-tags-contract` verifies exact listings, every individual selection,
misuse status codes, no output files in an external empty working directory, and
eight concurrent full invocations of each tier sharing that working directory.

### #287 Linux validation

- Release, GUI enabled: the full default build and all 82 CTest entries pass
  sequentially. The 81 entries excluding the legacy aggregate also pass with
  `-j 6`; the aggregate runs separately because of the pre-existing legacy
  save-transaction temporary-path overlap described under #282.
- A clean GUI-disabled Release build of `pf-smoke-agent-tags` compiles only its
  own runner/checks and smoke support among headless sources. Its link command
  contains only smoke support, core, YAML, and Lua. All 13 checks pass.
- A clean GUI-disabled Debug build with `PF_HIGH_ANALYSIS=ON` builds both tag
  tiers. All three focused CTest entries pass concurrently, including every
  single-check selection and eight concurrent full invocations per tier. Editor
  linkage contains only its own checks, headless support, editing, render, core,
  YAML, Lua, and CPU ImGui libraries. Existing elevated-analysis warnings remain.
- Mechanical assertion comparison confirms all 380 original `require` call
  sites remain in their new owners (one shared deserialization helper is copied
  into both tiers). No migrated definition or invocation remains in legacy code.
- `git diff --check` passes; no repository formatter is configured. New checks
  create no windows, native dialogs, system clipboard interactions, or ini/log
  files. Windows runtime validation remains the separate #279 ticket.

## Behaviours module (#288, #289)

```sh
cmake --build build-linux --target pf-smoke-behaviours pf-smoke-behaviours-editor --parallel
build-linux/bin/x64/Release/pf-smoke-behaviours --list
build-linux/bin/x64/Release/pf-smoke-behaviours --check definitionsPersistWithSchemasRevisionsAndModulePaths
build-linux/bin/x64/Release/pf-smoke-behaviours-editor --check reconcilesAndMigratesAcrossLoadedWorlds
ctest --test-dir build-linux -R '^smoke-behaviours' -j 3 --output-on-failure
```

The six former registry, assignment, portability, deletion, schema reconciliation,
and Workflow sources now register 26 checks individually, retaining their original
function names as stable selectors:

- `pf-smoke-behaviours` / `smoke-behaviours` (`smoke;core`) owns 38 registry,
  assignment-validation, deterministic Workflow, and runtime checks. It links only smoke
  support, production core, YAML, and Lua. No editor or rendering dependencies
  enter its check sources or link command.
- `pf-smoke-behaviours-editor` / `smoke-behaviours-editor` (`smoke;editor`) owns
  14 checks for coordinated registry recovery/reload, assignment authoring,
  clipboard/package portability, deletion, schema reconciliation, document history,
  and actual panel rendering. Mixed scenarios retain all their assertions together
  in this tier. Both domain CTest entries have 30-second timeouts.

The editor tier reuses production `pf-agent-editing` and headless render support.
Agent behaviour assignment and Marker panels now compile once in the shared editing
library rather than separately in GUI and legacy targets. Editor checks reset
panel state, pending confirmations, document history, logs, and write-failure
injection on entry and exit. Rendering uses scoped CPU-only ImGui contexts with
ini/log files disabled. Confirmations are answered through production seams:
no native dialogs, desktop windows, platform backends, or user input are needed.

Every temporary package lives in a distinct fixture directory under the invocation's
atomically reserved Context root, cleaned on success or exception. The contract
test verifies exact listings, every individual selection, misuse exit codes, no
working-directory output, and eight concurrent full invocations per tier.

All six original authoring sources and legacy declarations/calls are removed.
#289 also removes the runtime source and both legacy runtime invocations. The
empty `agent-behaviours` and `coordinated-document-checks` CTest registrations are
removed; their old CLI selections return 2 with migration diagnostics. Use CTest
for combined coverage.

### Runtime groups (#289)

The former 2,890-line runtime suite is physically split by responsibility. All
26 original runtime invocations are individually listed and selectable; both
Route-loss API versions have explicit selectors. The editor tier is unchanged.

| Source in `smoke/behaviours` | Checks | Responsibility |
| --- | ---: | --- |
| `RuntimePreflight.cpp` | 5 | Versioned contracts, diagnostics, sandbox surfaces, loader, registry status |
| `RuntimeContainment.cpp` | 3 | Scratch/live instruction and allocation budgets, protected recovery |
| `RuntimeInstances.cpp` | 2 | Independent startup state, immutable configuration, private helper graphs |
| `RuntimeMovement.cpp` | 4 | Bundled workflows, intent replacement, Route loss and topology (v1/v2) |
| `RuntimeCallbacks.cpp` | 4 | Movement ownership, activation, interaction payloads, best-effort teardown |
| `RuntimeScheduling.cpp` | 2 | Timer ordering, semantic state, configured schedules and seeded replay |
| `RuntimeFailures.cpp` | 1 | Callback/command storms, failure scope, bounded transactional logging |
| `RuntimeDeterminism.cpp` | 2 | Independent planning random streams, stable Lua iteration and identity output |
| `RuntimeScale.cpp` | 1 | Steady-state source-size scaling with 200 Agents |
| `RuntimeAuthorization.cpp` | 2 | Transient Permission grants and renamed-name diagnostics |

```sh
build-linux/bin/x64/Release/pf-smoke-behaviours --check routeLossAndTopologyLifecycleV2
build-linux/bin/x64/Release/pf-smoke-behaviours --check steadyStateBoundariesReuseSharedSources
```

Replay helpers stay local to their owning responsibility. Runtime fixtures use
Context-owned temporary directories, and bundled Lua sources resolve through
`Context::fixture()`, never the working directory or source-file location. The
scale threshold, sample sizes, replay digests, and all original assertions are
unchanged. Scale timings remain in failure diagnostics rather than introducing
non-contract stdout records.

### #289 Linux validation

- Release, GUI enabled: full default build and all 83 CTest entries pass
  sequentially; all 82 non-aggregate entries pass with `-j 6`. The aggregate runs
  separately from legacy serialization because of their existing path overlap.
- Clean Release, GUI disabled: building `pf-smoke-behaviours` alone compiles only
  its checks/runner and smoke support among headless sources, and links only
  support, production core, YAML, and Lua. Direct execution passes all 38 checks.
- Clean Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: both Behaviours targets build
  and all three focused CTest entries pass concurrently. The contract verifies
  exact listings, every selection, misuse, no working-directory output, and
  eight concurrent full invocations per tier. Existing analysis warnings remain.
- The pre-migration runtime selection and migrated runner both pass. Mechanical
  comparison preserves all 250 original `require` call sites and all 50 embedded
  Lua fixtures byte-for-byte (assertion whitespace normalized). No runtime source
  or invocation remains in legacy linkage; the retired CLI selection returns 2.
- `git diff --check` passes; no repository formatter is configured. Runtime checks
  link no editor/rendering code and cannot create windows or native dialogs.
  Windows runtime validation remains #279.

### #288 Linux validation

- Release, GUI enabled: full default build and all 84 CTest entries pass
  sequentially; all 83 non-aggregate entries pass with `-j 6`. The aggregate runs
  separately from legacy serialization to avoid their pre-existing temporary-path
  overlap.
- Clean Release, GUI disabled: building `pf-smoke-behaviours` alone produces only
  its runner, Registry, Assignment, Workflow, and smoke-support headless objects.
  Its link command contains only support, production core, YAML, and Lua. All
  12 checks pass.
- Clean Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: both Behaviours targets build
  and all three focused CTest entries pass concurrently, including individual
  selection and concurrent invocations. Existing elevated-analysis warnings remain.
- Mechanical comparison confirms every one of the 243 original assertion call
  sites is retained. Stable lists contain 12 core and 14 editor checks, each with
  one owner. No migrated invocation remains in the legacy executable.
- `git diff --check` passes; no repository formatter is configured. Windows
  runtime validation remains the separate #279 ticket.

## Permissions modules (#290)

```sh
cmake --build build-linux --target pf-smoke-permissions pf-smoke-permissions-editor --parallel
build-linux/bin/x64/Release/pf-smoke-permissions --list
build-linux/bin/x64/Release/pf-smoke-permissions --check authorizationAndPersistence
build-linux/bin/x64/Release/pf-smoke-permissions-editor --check destinationAuthoringShuttle
ctest --test-dir build-linux -R '^smoke-permissions' -j 3 --output-on-failure
```

Seven former legacy sources now have 97 individually selectable registrations:

- `pf-smoke-permissions` / `smoke-permissions` (`smoke;core`) owns 88 checks for
  Access permissions, Permission sets, manual and controlled Door authorization,
  destination authorization, Permission adherence, transport landing adherence,
  Interaction point mobility/geometry, Door preflight, and threshold refusals.
  It links only smoke support, production core, YAML, and Lua. No editor, renderer,
  ImGui, HTTP, legacy, or other module checks compile when building this target.
- `pf-smoke-permissions-editor` / `smoke-permissions-editor` (`smoke;editor`) owns
  nine checks for destination authoring/history/persistence, runtime property and
  Permission panels, adherence clipboard/history, and landing-profile history and
  Selection text for Lift, Platform lift, and Shuttle fixtures. It reuses
  `pf-agent-editing` and headless render support. `PermissionsPanel.cpp` now compiles
  once in the production editing library shared with the GUI and legacy target.

The former cross-file suite calls are replaced by peer registrations in each
runner. Core landing journeys use public registry mutations; their Editor peers
own the former undo/redo and display assertions, without duplicating journey
checks or introducing UI hooks into core checks. Public World, Graph, snapshots,
Events, serialization, and simulation ticks remain the assertion seams. All 549
original assertion call sites are retained. Mixed destination authoring checks
keep their history and persistence assertions together in the Editor tier.

Destination enforcement selectors end in `Lift0`–`Lift5`, `Platform0`–`Platform5`,
or `Shuttle0`–`Shuttle5`: unchanged authorization, onboard runtime revoke, onboard
requirement tightening, onboard authored revoke, onboard adherence change, and
pre-boarding adherence change. Landing journey selectors use the same transport
names with variants 0–7: inherited opportunism, stale Path, revoke riding, revoke
boarding, revoke before boarding, tighten before boarding, tighten riding, and
enable adherence riding. Alternatives have separate selectors for each transport.

All checks use in-memory Worlds. Editor checks reset panel/history state and scope
registry history and CPU ImGui contexts, including exceptional exits. Ini/log
files are disabled, clipboard capture stays in process, and there are no platform
backends, graphics windows, native dialogs, or required user input. The contract
checks exact listings, every single selection, misuse status codes, no files in
an external empty working directory, and eight simultaneous runs of each tier.
Core and Editor CTest timeouts are 60 and 30 seconds; the contract has 180 seconds
for Debug and concurrent execution.

The seven old sources, declarations, and invocations are removed from legacy
compilation/execution. The empty `access-permissions` CTest entry is retired;
`--access-permission-checks` returns 2 with migration guidance. Inline scenarios,
compile-only API contracts, and compatibility dispatch remain in their separate
follow-up tickets rather than extending #290.

### #290 Linux validation

- Release, GUI enabled: full default build and all 85 CTest entries pass
  sequentially. All 84 non-aggregate entries pass with `-j 6`; the aggregate runs
  separately from legacy serialization because of their existing fixed-path
  overlap. The original Access permission selection passed before extraction.
- Clean Release, GUI disabled: building only `pf-smoke-permissions` produces only
  its runner/eight check objects and smoke support among headless sources. Its
  actual link command contains only support, production core, YAML, and Lua.
  Direct execution passes all 88 checks.
- Clean Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: both targets build and all
  three focused CTest entries pass concurrently, including every single selection
  and eight concurrent full runs per tier. Existing elevated-analysis warnings
  remain. Observed focused CTest time was about 77 seconds (Release: 19 seconds).
- Mechanical comparison preserves all 549 original `require` assertion calls
  (whitespace normalized). `git diff --check` passes; no repository formatter is
  configured. Windows execution remains the separate #279 ticket.

## Routing scale and perceived route costs (#293)

```sh
cmake --build build-linux --target pf-smoke-routing --parallel
build-linux/bin/x64/Release/pf-smoke-routing --check populationRouting
build-linux/bin/x64/Release/pf-smoke-routing --check capturedInputsMatchEagerProviders
build-linux/bin/x64/Release/pf-smoke-routing --check actualPositionChoosesManualAlternative
ctest --test-dir build-linux -R '^smoke-routing' -j 3 --output-on-failure
```

Routing adds 31 individual registrations to the 40 from #291:

| Source in `smoke/routing` | Checks | Responsibility |
| --- | ---: | --- |
| `Workspace.cpp` | 12 | Workspace reuse, reference Dijkstra, captured facts, source indexes, lower bounds, local demand, observation epochs, reset and population scale |
| `ThresholdRouteCost.cpp` | 4 | Actual-position Paths, walking/Bulkhead facts, observed queues, Door alternatives |
| `StairRouteCost.cpp` | 4 | Directed physical stair costs, speed/effort preferences, multi-flight Stairwells |
| `LiftRouteCost.cpp` | 5 | Admission/ride separation, queue epochs, waiting/crowd preferences, Platform lifts |
| `ShuttleRouteCost.cpp` | 1 | Complete short/long journey, capacity, dwell, local crowd and preview scenario |
| `LadderForceBridgeRouteCost.cpp` | 5 | Physical climbing, local/remote deployment, speed/risk preferences and Force Bridge controls |

All required checked-in Worlds resolve through `Context::fixture()`, including the
threshold regression fixture. Every single selector and eight concurrent complete
invocations run from an external empty working directory in the Routing contract.
Missing fixtures fail rather than skip. Core timeout is 120 seconds; the contract
has 300 seconds for Debug/concurrent workloads. Editor ownership is unchanged.

Smoke preserves the original workloads and assertions, including 1,000 Agents,
2,040 population vertices, cold/warm/reset/fresh Path digests, allocation bounds,
reference oracles, and all preference and physical-duration outcomes. There are
no elapsed-time pass/fail thresholds. Informational benchmark output is removed
from smoke, which emits only harness records. The restoration benchmark and
`--write-routing-scale-world` remain explicit legacy tools in `RoutingTools.cpp`.
Population construction/assertions compile once in `pf-routing-population-support`;
only the export tool enables file output and timing/memory reporting. The Routing
module links only that support, smoke support, and production core/YAML/Lua, not
legacy tooling or other modules' checks. No production routing code changes.

The six legacy sources and suite calls are removed. `--routing-scale-checks` and
`--shuttle-route-checks` return 2 with migration guidance; use CTest for combined
coverage. The benchmark/export CLI remains compatible and is not registered as a
Routing smoke check.

### #293 Linux validation

- GUI-enabled Release default build and all 87 CTest entries pass sequentially;
  all 86 non-aggregate entries also pass with `-j 6`, avoiding the documented
  legacy aggregate/serialization temporary-path overlap.
- Clean GUI-disabled Release builds only `pf-smoke-routing` and passes all 71
  core checks. Actual objects and link commands contain only Routing, the two
  support libraries, and core/YAML/Lua; no editor, renderer, ImGui, HTTP, legacy
  checks, or other module checks compile.
- Clean GUI-disabled Debug with `PF_HIGH_ANALYSIS=ON` builds Routing, Routing
  Editor, and legacy headless. All three Routing tests pass concurrently (about
  75 seconds), including exact listings, all selectors and concurrent execution.
  Legacy aggregate, serialization, rendering and restoration tests also pass.
- Explicit export of the population World and adjacent tag registry, followed
  by two restoration benchmark cycles, passes from an external working directory.
  Retired smoke selections return 2. All 241 original assertion call sites remain
  in the migrated checks/shared workload/explicit tools (whitespace normalized).
- `git diff --check` passes; no formatter is configured. Checks use no graphics
  backends, windows, dialogs or interactive input. Windows runtime remains #279.

## Routing modules (#291)

```sh
cmake --build build-linux --target pf-smoke-routing pf-smoke-routing-editor pf-smoke-render --parallel
build-linux/bin/x64/Release/pf-smoke-routing --list
build-linux/bin/x64/Release/pf-smoke-routing --check ordinaryCommands
build-linux/bin/x64/Release/pf-smoke-routing-editor --check boundariesAndPresentation
build-linux/bin/x64/Release/pf-smoke-render --check agentPaths
ctest --test-dir build-linux -R '^smoke-(routing|render)' -j 4 --output-on-failure
```

Seven former legacy sources now have 50 individually selectable registrations:

- `pf-smoke-routing` / `smoke-routing` (`smoke;core`) owns 40 checks for planning
  timers, deterministic streams, deferred outcomes, interruptions, topology and
  assigned-idle restoration, queue planning, planning-time properties, movement
  commands, Mobility constraints, saved Paths, and isolated-Sector pathing.
  It links only smoke support, production core, YAML, and Lua.
- `pf-smoke-routing-editor` / `smoke-routing-editor` (`smoke;editor`) owns nine
  mixed scenarios using clipboard serialization, Selection-panel presentation,
  or World/registry history. Complete original scenarios remain together so their
  timer, persistence, authorization, and clipboard assertions stay unchanged.
  It reuses production editing and headless rendering support, not native UI.
- `pf-smoke-render` gains `agentPaths`: selected Path geometry and planning/queue
  badge appearance, lifetime, stacking, and debug gating. Its UI settings and
  selection are restored even on failure. ImGui contexts are CPU-only and scoped,
  with ini/log output disabled. No graphics window or dialog is created.

All three domain tests have 30-second timeouts. Every parameterized planning
variant and Lift cancellation boundary has a stable selector; cancellation still
compares two journeys for determinism. Original assertion call sites are retained
(347 total, with shared fixture helpers copied where dependency tiers split).
Editor checks reset tag-panel and World-history state on entry and exit.

Restoration fixtures use distinct per-check directories below the harness's
unique invocation root, cleaned on success or failure. Other fixtures are built
in memory; no source or working-directory fixture lookup is required. The Routing
contract checks exact listings, every selection, misuse, no files in an external
empty working directory, and eight concurrent invocations of each tier. Render's
contract now also selects `agentPaths` independently.

The seven legacy files, declarations, and calls are removed. The empty planning
CTest entries are retired; the old planning, planning-time, and restored-Path CLI
selections return 2 with migration guidance. Route-cost, workspace, scale, and
inline checks remain outside #291; route-cost, workspace and scale checks
subsequently migrate in #293 above.

### #291 Linux validation

- Release, GUI enabled: full default build and all 86 CTest entries pass
  sequentially. All 85 non-aggregate entries pass with `-j 6`; the aggregate runs
  separately from legacy serialization to avoid their existing fixed-path overlap.
- Clean Release, GUI disabled: building only `pf-smoke-routing` compiles just its
  runner/six check sources and smoke support among headless sources. Its actual
  link command contains only support, production core, YAML, and Lua. All 40
  checks pass; no editor, renderer, ImGui, legacy, or other module checks compile.
- Clean Debug, GUI disabled, `PF_HIGH_ANALYSIS=ON`: both Routing targets and
  Render build; all five focused domain/contract tests pass concurrently,
  including every selection and concurrent invocation checks. Existing
  elevated-analysis diagnostics remain warnings.
- Both original planning CLI selections passed before extraction. Mechanical
  comparison confirms all 347 assertion call sites remain, normalizing whitespace
  and expanding the four cancellation boundary values. No migrated legacy symbol
  remains. `git diff --check` passes; no repository formatter is configured.
  Windows runtime validation remains the separate #279 ticket.

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
