# Headless simulation smoke scenario

The `core` target builds the shared simulation as `prometheum-fermide-core.lib`. Both the `headless` and `editor` projects reference that static library, so simulation sources are compiled once per configuration instead of being duplicated in each executable. The `headless` target builds deterministic smoke scenarios without SDL, ImGui, OpenGL, rendering, or audio dependencies. The scenarios route agents between marker vertices, verify ordinary request/permit/commit behavior, denial and cancellation, exercise manual, automatic, remote-controlled, unavailable, fair two-sided queued, and wide concurrent doors, verify scoped open leases, sensor-driven reopening, graceful deactivation, queue cancellation, physical waiting separation, logical queue overflow, compatible replan priority, deterministic permit expiry/reassignment, same-layer bulkhead coordination, conditional open-window traversal, and atomic paused topology rebuilds with ownership cleanup and failure diagnostics, advance worlds in fixed ticks, and exit unsuccessfully if an invariant fails or two identical runs produce different snapshots or events.

## Walking and threshold route costs (#206)

`ThresholdRouteCostSmokeChecks.cpp` covers physical walking and Bulkhead Door
crossing distance, Walk speed scaling, Door crossing and opening preparation,
automatic/manual/remote interaction costs, approach-side queue charging, and
unobserved state/queue isolation. Paired alternatives reproduce #206's manual
check: an open automatic Door beats a nearby closed remote Door; opening the
remote Door makes the shorter route win. All four opening styles, including a
tall Door, retain the same costs and selected Path.

The saved `realistic-pathing-test.world.yaml` also exercises inferred-source
routing: the Agent's actual position splits the ordinary floor edge beneath it,
so both approach directions compete with their physical walking costs. The
search does not first force a visit to the nearest vertex. Reconstruction retains
that initial approach in cumulative cost and duration; explicit-source queries
still start at zero. The scenario checks the manual alternative, repeat-query
determinism, warmed scratch reuse, and arrival at the saved destination.

These arcs expose motion and estimated objective duration separately from
perceived inconvenience. The query captures effective Walk speed and the Agent's
current Sector. Only thresholds approached from that Sector reveal state and
queue delay; other thresholds (including null-Agent previews) use policy
expectations. Defaults assume a 50% chance of needing opening preparation and
no unknown queue delay. Threshold/manual/remote interaction defaults are
0.05/0.5/2 seconds-equivalent. Opening preparation uses style-independent nominal
timing; visual animation timing remains unchanged. Unmigrated modes retain the
compatibility adapter.

## Shuttle journey route costs (#215)

`ShuttleRouteCostSmokeChecks.cpp` covers actual stop-distance/speed ride time,
per-departure dwell (including intermediate service), one boarding charge,
capacity-dependent missed service, local queue observations, remote-state
isolation, independent aversions, and short/long walking alternatives. Run it
alone with `prometheum-fermide-headless.exe --shuttle-route-checks`.

Landing Doors charge boarding/alighting and interaction; Shuttle body arcs never
repeat admission. Travel between Doors at the same Stop is walking, not a ride.
The baseline service interval is 12 seconds: admission expects half an interval
plus one interval per capacity-sized expected queue. Unknown queues default to
one passenger; unknown crowding defaults to 0.2. Capacity sums each Carriage
reachable from the connected access zone once, regardless of its Door count.
Local observations replace these queue/crowding expectations, but never inspect
remote vehicle position, passenger manifests, or scheduled requests. Alighting
does not pay another service wait. Null-Agent previews retain authored facts.

The distance crossover fixture uses an Agent with a 0.8 Walk speed modifier:
the current default Shuttle speed equals neutral walking speed, so a neutral
Agent should not prefer it over unobstructed parallel walking merely because
the trip is long. No simulation movement speeds are changed by this ticket.

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

## Lift occupant clearance (#177)

Enclosed Lift capacity positions use the World's `occupantClearance`. Compact
packing centres the full authored capacity in the physical car, uses the requested
adjacent-occupant gap where it fits, and otherwise shares the complete body-safe
span without dropping slots. Changing the World geometry policy also updates an
already-authored Lift and its car-side boarding lanes.

`occupant-packing` checks exact zero-clearance compatibility, full configured
clearance in a wide car, and maximum possible spacing in tight and partially tight
cars. `liftOccupantsUseWorldClearance` checks the resulting Lift resource snapshot
and verifies that authored capacity is unchanged. The paired-run simulation digest
was deliberately regenerated in Debug and Release; both runs matched. The recorded
queue digests above are unaffected.

## Lift occupant ordering (#178)

Enclosed Lift cars reserve free capacity positions from the end farthest from the
Doors. Occupants with the same destination retain boarding order from farthest to
nearest; occupants with different destinations are ordered so the Stop nearest
along the current direction of travel is nearest the Doors. Equal destination
comparisons preserve the deterministic manifest order established by stable
capacity positions and traversal admission.

`liftOccupantsAreOrderedByBoardingAndDestination` checks both rules through the
simulation snapshot and then allows the existing finite-capacity journey check to
cover boarding reservations, exact queue-position arrival, filling, and departure.
The paired-run simulation digest was deliberately regenerated in Debug and Release;
both runs matched. The recorded queue digests above are unaffected.

## Lift occupant re-spacing (#179)

While an enclosed Lift occupant alights, the occupants staying in the car receive
compact targets across the body-safe car extent. Capacity-slot ownership and
logical ordering do not change, and Agent locomotion reaches the new targets at
ordinary walk speed. The per-Agent targets remain attached to the car while it
moves, so moving occupants have no walking goal and are carried without a
horizontal snap. A later boarder restores the destination-ordered capacity layout
before entering.

`liftOccupantsRespaceWhileAnOccupantAlights` observes the complete alighting
window, verifies that the remaining occupants' snapshot targets change, bounds
each stopped-car position change by walk speed, confirms that the alighting Agent
reaches its destination, and requires the Lift to depart within the normal stop
phases. The paired-run simulation digest was deliberately rerun in Debug and
Release; both runs matched. The recorded queue digests above are unaffected.

## Shuttle door-aware placement (#180)

A Shuttle occupant selects an alighting Door while boarding and retains that Door
for the transport journey. Occupant packing divides each Carriage into buffered
Door-owned sub-ranges, preserving capacity and stable boarding order while keeping
an occupant near the Door it will use.

`shuttlePassengerUsesBoardingSelectedAlightingDoor` checks the retained Door,
Door-consistent grouping and the distance walked while alighting. The existing
Carriage spreading, capacity, access-zone and transport-journey checks remain
active. This changed only Shuttle geometry, so the queue trace digests above are
unchanged.

## Geometry close-out (#181)

The complete Release smoke suite records the following Linux/GCC digests. Paired
runs construct the same World with random seed 0 and advance the same 60 ticks;
the second 500-Agent run also enables metrics to prove that observation does not
change simulation output.

| Digest | Final value |
| --- | ---: |
| 500-Agent event stream | 13886706955275287569 |
| 500-Agent final snapshot | 7023572238893341247 |
| Queue, separation 0.5, direction -1 | 12044598890430814109 |
| Queue, separation 0.5, direction +1 | 6254148168206110256 |
| Queue, separation 0.8, direction -1 | 16720789843228646775 |
| Queue, separation 0.8, direction +1 | 14720704966384623342 |
| Overflow queue | 6611208500844518019 |

The queue values deliberately changed in #172 and #175. The Lift and Shuttle work
has separate observable assertions and does not execute in those queue traces, so
it has no reason to alter their values. The event and snapshot digests belong to
the scale World, whose ordinary Corridor movement is likewise independent of the
new queue and vehicle geometry. Repeating the complete binary produced the same
seven digest values.

The scale fixture was measured before the geometry series at `01dff23` and after
it at `e5ad994`. Each value below is the median of three alternating Release runs
on Linux 7.0, GCC 15.2, and an AMD Ryzen AI MAX+ 395. Working set is sampled by the
process after each timed run. These are observations rather than portable limits.

| Active Agents | Measurement | Before geometry | After geometry | Change |
| ---: | --- | ---: | ---: | ---: |
| 500 | 60-tick elapsed time | 14.3396 ms | 14.2571 ms | -0.6% |
| 500 | working set | 131.469 MiB | 132.961 MiB | +1.1% |
| 1000 | 60-tick elapsed time | 47.4967 ms | 46.9249 ms | -1.2% |
| 1000 | working set | 149.906 MiB | 151.598 MiB | +1.1% |

There is no material time or memory regression. Geometry updates assign walking
targets; the queue checks and Lift/Shuttle checks bound stopped-Agent movement by
walk speed. Direct position updates remain confined to a moving transport vehicle
carrying its occupants. Queue state is not consulted by pathfinding and introduces
no general collision avoidance, so an unrelated Agent's Path is not displaced by
waiting Agents.

## Perceived-cost routing (#205)

Routing now uses Dijkstra with graph-slot tie-breaking, retaining the reusable
Graph-owned workspace. `RouteCost.h` separates explicit feasibility, validated
non-negative cost components, optional objective duration, and perceived cost.
A World owns the runtime `RouteChoicePolicy`; each synchronous query copies its
policy/profile into a const `RouteDecisionContext` and captures both directions
of every arc before expanding the frontier. Relaxation reads only this snapshot.
Snapshot buffers are reused and included in scratch-allocation accounting.

`Edge::getDirectedTraversalFacts` is the migration seam. Its default adapter
retains existing `getWeight(..., true)` behaviour, including Mobility checks and
legacy finite-sentinel/+infinity exclusions. New facts use `feasible=false` for
exclusions; even a perceived cost above the old sentinel remains usable. Invalid
components, totals, or cumulative scores are rejected with `invalid_argument`.
The compatibility adapter does not claim that mixed legacy weights are objective
time: cumulative objective duration is unavailable until all traversed edges
supply it. `PathNode::edgeWeight` remains the cumulative perceived score for
compatibility, also exposed as `getCumulativePerceivedCost()`.

The initial capture is O(E) per query and still performs legacy tag/live-state
lookups. Compact authored facts, effective physical profiles, local-only
observations, and traversal-specific timing are subsequent migration work; no
claim of local-only knowledge is made for the compatibility adapter. Actual
movement and traversal coordination continue using their existing timing APIs.

`PathfindingWorkspaceSmokeChecks` checks all vertex pairs in the four bundled
#191 Worlds against an independent reference Dijkstra, component validation,
finite dislike versus exclusion, graph-slot ties, null-Agent previews,
source-equals-target, unreachable/stale workspace behaviour, and warmed scratch
allocation stability. Existing Mobility routing and runtime-gate tests remain
active.

## Population routing (#224)

Run `prometheum-fermide-headless.exe --routing-scale-checks` for independent
reference-Dijkstra checks, source-inference boundary checks, and a generated
mixed population: 1,000 Agents, 2,040 Vertices, four Levels, stationary
Staircases, Escalators, a Ladder, and a Lift. Profiles include defaults,
shared-tag properties, and unquantised individual properties. Timed batches
include actual source inference and cross-Level destinations, and must use
multiple traversal kinds. Cold, warm, World-reset, and independently rebuilt
Worlds must reproduce the same Path/cost digest. Warm scratch allocations and
immutable-geometry rebuild counts must not grow. Separate tests exercise
uncertain route costs across reset, profile extremes, cache eviction, topology
invalidation, and observation epochs across ticks, cancellation and activation.

The Graph retains directed adjacency, lengths, rises, floor intervals, floor
segments, and deterministic Graph-local perception identities. Source inference
no longer repeatedly walks floor cells for every progressively closer Marker.
Authored device speeds/timings remain scalar device facts; Shuttle access-zone
capacities are cached independently of queue changes. Effective properties and
their diagnostic provenance are resolved together once; Mobility is shared by
both feasibility passes. Relaxation reads only captured costs and compact arcs:
no tag/registry scans, queue walks, weak endpoint locks, or scratch allocation.

Lift/Shuttle queue snapshots update on admission membership epochs. Escalator
standing counts update on admission, cancellation, cleanup and activation—not
population scans. Door observations compare the exact scalar state of their two
approaches and publish a new epoch only when that state changes. This preserves
local visibility without tick-wide invalidation or approximate queue buckets.
Runtime admission and physical motion are unchanged.

A four-entry, Graph-local target LRU caches reverse shortest-path lower bounds.
Walking uses maximum valid Walk speed; every other arc is relaxed to zero,
including fast transport and forbidden directions. Ignoring feasibility and
blocking Markers can only lower these bounds. Conservative rounding allowances
cover both reverse sums and forward float accumulation; zero remains the safe
fallback. All four tables reserve storage at warm-up, so eviction allocates no
scratch. Profiles, policy and observations do not enter this universal bound
cache. No route-result cache or property quantisation is used.

A Windows/MSVC Release run observed approximately 9,000–10,000 Paths/second,
645,848 workspace bytes, four target builds and 1,996 hits after the warm batch,
and digest `16756504350970399886`. Working set was about 20–22 MiB for routing
and 26 MiB after serialization-based reset. These are observations, not portable
timing or peak-memory limits. Reset serialization is outside the routing timer.
The complete Release editor/headless build and headless suite passed, with
unchanged simulation event/snapshot digests.

To generate an editor-loadable fixture and its adjacent tag registry:

```bat
prometheum-fermide-headless.exe --write-routing-scale-world routing-scale.world.yaml
```

The command refuses existing output files, assigns varied destinations, and
checks document round-trip loading. Open the World in the editor, resume, reset,
and rerun to assess UI responsiveness; interactive UI verification is still
manual. Large-World serialization/reset overhead is not measured as routing
throughput.

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

Use `Release` instead of `Debug` for an optimized build. CMake places final executables and libraries in `bin\x64\<Configuration>` within its build directory (for example, `out\build\bin\x64\Debug`). CMake fetches and statically links the graphical dependencies, then copies only `prometheum-fermide.ini` beside `editor.exe`.

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

Build the shared static library, graphical application, and headless target through CMake:

```bat
cmake --build build-windows --config Debug --parallel
cmake --build build-windows --config Release --parallel
```

The graphical target is `editor`. CMake generates `editor.vcxproj` and the `editor.dir` intermediate directory on Windows. The executable is `bin\x64\<Configuration>\editor.exe` within the build directory (`editor` on Linux), with the existing runtime resource and DLL copy steps.
