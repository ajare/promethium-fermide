# Headless simulation smoke scenario

The `core` target builds the shared simulation as `prometheum-fermide-core.lib`. Both the `headless` and `imgui` projects reference that static library, so simulation sources are compiled once per configuration instead of being duplicated in each executable. The `headless` target builds deterministic smoke scenarios without SDL, ImGui, OpenGL, rendering, or audio dependencies. The scenarios route agents between marker vertices, verify ordinary request/permit/commit behavior, denial and cancellation, exercise manual, automatic, remote-controlled, unavailable, fair two-sided queued, and wide concurrent doors, verify scoped open leases, sensor-driven reopening, graceful deactivation, queue cancellation, physical waiting separation, logical queue overflow, compatible replan priority, deterministic permit expiry/reassignment, same-layer bulkhead coordination, conditional open-window traversal, and atomic paused topology rebuilds with ownership cleanup and failure diagnostics, advance worlds in fixed ticks, and exit unsuccessfully if an invariant fails or two identical runs produce different snapshots or events.

## Deterministic simulation API

`World::advanceTick()` and `World::advanceTicks()` are the headless seam. Each tick is `World::getFixedTimestep()` (1/60 second) and runs these phases in order:

1. resource advancement;
2. intent collection;
3. allocation;
4. movement;
5. commit;
6. cleanup and event publication.

The traversal protocol now uses those seams directly. On reaching an edge, an agent creates one world-owned request during intent collection. Allocation grants an immediate permit for an unconstrained edge, movement advances the agent between the edge's path vertices while it remains a source-sector occupant, commit transfers sector membership at the destination endpoint, and cleanup releases the transaction records. Denied requests remain blocked, and cancelling a path releases its request and permit without committing. Resource updates and stable-ID-ordered agent updates remain separated.

`World::getSimulationSnapshot()` returns a value snapshot containing the tick and stable agent IDs, sectors, positions, path state, path progress, active locomotion task, traversal request, and traversal permit. Every traversal request also has a human-readable `diagnostic` describing its current wait, active permit, denial, cancellation, or commit outcome. It also exposes the world-owned interaction points, device operations, traversal resources, requests, and permits by stable typed ID. Door-resource snapshots include activation mode, enabled state, open state and percentage, typed open-lease counts, presence and obstruction observations, generated queue lanes and their position owners, and every independent crossing lane and owner. Door requests expose separate logical queue tickets, optional physical queue-position reservations, and their typed opening operation. Ladder-resource snapshots expose capacity derived from crossed levels and `CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO`, logical admission order, distinct climbing positions, occupants, admission reservations, active direction, bounded-batch progress, and per-direction waiting demand. Extensible ladder and force-bridge resources additionally expose desired-state preparation, pending safe retraction, and independently owned request and occupant extension leases. Ordinary stairs remain unconstrained; narrow stairs opt into the same directional capacity policy and diagnostics. Lift-resource snapshots expose alignment, motion, target stop, LOOK direction, active scheduled stops and their oldest request ticks, explicit stop phase and dwell/cutoff timing, car-door interlock, all admission and standing-position slots, destination-confirmation order, and per-stop request-owner counts. The lift scenarios verify physical landing calls, finite simultaneous boarding capacity, overflow demand retained for a later run, cutoff-safe reservations, separate boarding/ride/disembark path edges, attached passenger motion, serialized destination selection, door interlocks, final disembarkation, coalesced request ownership, direction-compatible boarding, deterministic LOOK ordering, non-uniform stop spacing, bounded destination-selector retries, safe cancellation exits, disabled-lift draining without stale ownership, single-carriage shuttle calls, finite capacity, attached horizontal travel, disembark-first stop service, and later service for overflow passengers, plus coupled-shuttle per-carriage manifests, disconnected access-zone queues, deterministic door assignment, and individual carriage capacity enforcement. Open platform lifts reuse the lift manifest, destination, dwell, cutoff, cancellation, deactivation, and LOOK policies while exposing a virtual boundary instead of physical door resources; the headless journey verifies boundary interlocking, finite capacity, destination confirmation, and passenger attachment. `World::consumeSimulationEvents()` returns and clears value events; no event callback runs during a simulation phase. Phase-completion and traversal-lifecycle events make tick ordering observable.

Agents can be created with `World::createAgent()` and resolved with `lookupAgent()`. Replacement interaction points, device operations, and traversal resources follow the same create/lookup/remove pattern. Each category has a distinct handle type, lookups return an explicit diagnostic on invalid or removed handles, and IDs are never reused. The raw-pointer `addAgentToSector()` overload remains only as a legacy ownership-transfer seam during migration.

The graphical application continues to call `World::update(elapsedSeconds)`. That method accumulates render-frame time and advances only complete fixed ticks, so frame rate no longer determines simulation progress or operation completion.

Runtime structural changes use `pauseSimulation()`, the existing construction/configuration APIs, `rebuildTraversalTopology()`, and `resumeSimulation()`. Pausing freezes tick advancement, cancels active edge transactions at a safe source boundary, releases their queues, positions, permits, leases, and admission reservations, and retains destination intent. Graph construction and traversal-resource validation happen against a candidate graph; only a successful candidate replaces the active graph. A failed candidate leaves the simulation paused, keeps the previous graph installed, and exposes its diagnostic through the snapshot and `getTopologyDiagnostic()`. Successful rebuilds replan retained destinations against the replacement graph. Structural APIs reject edits outside this paused protocol after the initial build.

## Follow-the-leader queues (#172)

Reserved queue positions and ticket/admission order are unchanged. Before the
Movement phase, each reserved waiter receives a physical standing target behind
its predecessor's pre-movement position. Left and right approaches are separate;
a waiter at the Threshold anchors both. Separation is at least
`CORE_DOOR_QUEUE_STOP_WIDTH`, or `minimumQueueSeparation` when larger. Forward
retargeting uses `advanceStepThreshold`; a promoted head always targets its exact
reservation so Lift landing arrival remains possible. A follower keeps its existing
walk target while the cumulative forward advance is at or below that threshold,
and observing an unchanged target does not update `positionAssignedAtTick`.
Agents walk to these targets; no positions are assigned directly. Approaching
Agents join behind the physical tail rather than its already-compacted reservation.
Waiting at a chain target does not count as a local-goal timeout.

Request snapshots distinguish `queuePositionTarget` (reservation) from the
optional `queueStandingTarget` (physical walk target).

## Overflow tail targets (#175)

Every pending queue ticket has an observable standing target, even after all
reserved queue positions are occupied. Unreserved Agents extend the physical tail
by `overflowTailSeparation`; this changes neither ticket order nor admission.
Targets are clamped to the contiguous walkable floor behind the queue. Once that
floor is full, an Agent already farther from the Threshold keeps its current
position as its target rather than being drawn forward. Overflow does not run the
reserved-position timeout and cannot itself cause denial. Operators, missed
boarders, and positioned Agents in a retry delay expose their current position as
a target without receiving a competing walking goal.

`overflowingQueueAlwaysHasWalkableTailTargets` forces eight Agents through a lane
with one reserved position and checks every target on every observed tick, floor
clamping, order behind the reserved position, no overflow retries, and repeat-run
determinism. Its Linux/GCC trace digest is `6611208500844518019`.
Arbitrarily overlapping initial spawns still need time to walk apart.

`queueChainsFollowWithoutCompressing` checks actual and target separation every
tick of a complete two-sided Door service, delayed advancement after serving the
head, the explicitly configured 0.2-unit advance hysteresis boundary, stable
assignment ticks during a calm multi-Agent queue, walk-speed bounds, and identical
repeated traces. The existing fairness, cancellation, timeout, Lift boarding and
Shuttle checks also remain enabled.
The suite's existing determinism digests are generated from paired runs, not
stored golden files; they were deliberately rerun with the new geometry. New
queue trace digests (positions and states every tick, Linux/GCC) are recorded here:

| Separation | Approach direction | Trace digest |
| --- | --- | --- |
| 0.5 | -1 | 12044598890430814109 |
| 0.5 | +1 | 6254148168206110256 |
| 0.8 | -1 | 16720789843228646775 |
| 0.8 | +1 | 14720704966384623342 |

## Prerequisites

- Windows x64
- Visual Studio with the MSVC `v145` toolset and Windows 10 SDK
- CMake 3.25 or newer (for the CMake workflow)
- Git, with repository submodules initialized recursively

Run commands from the repository root in a Developer Command Prompt. Before
configuring an existing clone, populate Willpower and its nested dependencies:

```bat
git submodule update --init --recursive
```

## CMake build

Configure a 64-bit Visual Studio build tree:

```bat
cmake -S . -B out\build -A x64
```

Build and run only the headless smoke scenario:

```bat
cmake --build out\build --config Debug --target prometheum-fermide-headless
out\build\bin\x64\Debug\prometheum-fermide-headless.exe
```

Build all targets and run the registered CTest smoke test:

```bat
cmake --build out\build --config Debug
ctest --test-dir out\build -C Debug --output-on-failure
```

Use `Release` instead of `Debug` for an optimized build. CMake places final executables and libraries in `bin\x64\<Configuration>` within its build directory (for example, `out\build\bin\x64\Debug`). CMake fetches and statically links the graphical dependencies, then copies only `prometheum-fermide.ini` beside `imgui.exe`.

## Build and run only the headless scenario with MSBuild

```bat
msbuild build\headless.vcxproj /m /p:Configuration=Debug /p:Platform=x64
bin\x64\Debug\prometheum-fermide-headless.exe
```

Use `Release` in both paths to build and run the optimized configuration:

```bat
msbuild build\headless.vcxproj /m /p:Configuration=Release /p:Platform=x64
bin\x64\Release\prometheum-fermide-headless.exe
```

World `headless.vcxproj` automatically builds its `core.vcxproj` project reference. A successful run prints scale observations followed by a `PASS` line and returns exit code 0. The representative run advances 500 active agents alongside 32 resources twice and compares deterministic event/snapshot digests. A 1,000-agent stretch run records elapsed time and process working-set memory. These observations are deliberately informational rather than machine-dependent performance thresholds; both runs still enforce ownership and capacity invariants. A failed assertion prints a `FAIL` line and returns a nonzero exit code.

## Build the complete solution

The shared static library, graphical application, and headless target are all in `build\imgui.sln`:

```bat
msbuild build\imgui.sln /m /p:Configuration=Debug /p:Platform=x64
msbuild build\imgui.sln /m /p:Configuration=Release /p:Platform=x64
```

The graphical executable remains `bin\x64\<Configuration>\imgui.exe` and retains its existing runtime resource and DLL copy steps.
