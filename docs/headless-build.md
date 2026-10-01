# Headless simulation smoke scenario

Simulation Observation, Render walls, and Persistence serializer/document formats
now run in the independently buildable [`pf-smoke-*` modules](smoke-modules.md),
not the legacy aggregate below. The complete serialization/restoration suite now
runs in `pf-smoke-persistence`, with rendering-policy assertions in `pf-smoke-render`.
`--serialization-checks` returns 2 with migration guidance. The explicit
`--restoration-benchmark` tool remains available but is not a smoke CTest owner.
Use CTest for combined coverage during the incremental migration; see the
[ownership manifest](smoke-migration-manifest.md).

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
timing; visual animation timing remains unchanged. Every traversal now supplies
explicit directed facts; there is no combined Edge-weight compatibility path.

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

Runtime structural changes use `pauseSimulation()`, the existing construction/configuration APIs, `rebuildTraversalTopology()`, and `resumeSimulation()`. Pausing freezes tick advancement, cancels active edge transactions at a safe source boundary, releases their queues, positions, permits, leases, and admission reservations, and retains destination intent. Graph construction and traversal-resource validation happen against a candidate graph; only a successful candidate replaces the active graph. A failed candidate leaves the simulation paused, keeps the previous graph installed, and exposes its diagnostic through the snapshot and `getTopologyDiagnostic()`. Successful rebuilds retain destinations for timed Route planning against the replacement graph; an episode already in progress keeps its total and remaining ticks. Structural APIs reject edits outside this paused protocol after the initial build.

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

`Edge::getDirectedTraversalFacts` is the sole route-query contract. Every Edge
supplies explicit directed feasibility, objective duration, and cost components;
exclusions use `feasible=false`, never finite sentinels or infinity. Even a
perceived cost above historical sentinel values remains usable. Invalid
components, totals, or cumulative scores are rejected with `invalid_argument`.
`PathNode::cumulativePerceivedCost` stores only the cumulative perceived score,
also exposed as `getCumulativePerceivedCost()`.

The initial capture is O(E) per query. Compact authored facts, effective physical
profiles, local-only observations, and traversal-specific timing keep routing
separate from runtime movement. Actual movement and traversal coordination use
physical traversal speeds, device timing, reservations, and permits rather than
perceived cost.

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

## Demand-driven routing (#230)

Normal searches now start one value-only decision snapshot, then evaluate/cache
only requested directed arcs. The Graph owns generation-stamped cost and
exclusion slots; there is no per-Agent Graph or weight matrix. Walking consumes
cached lengths without a per-query arc pre-pass. Non-walking arcs capture scalar
authored inputs and permitted observations before expansion; this remains
O(non-walking arcs), is explicitly counted, and does not apply Agent-specific
cost formulae. `RouteTraversalInputs::evaluate` cannot read an Agent, World,
registry, queue, or device. All properties, policy and perception identity are
copied once. Fallback admission retains the snapshot but advances the cost
cache generation, so first-pass exclusions cannot leak into the second pass.

`captureRouteCosts` remains an explicitly eager diagnostic/test seam, not the
normal routing entry point. The older Edge fact providers remain available as
an independent differential oracle. Tests compare every component, feasibility
and duration across nine bundled Worlds, local/remote observations, profile
extremes and fallback admission. Frozen-input tests change queues *before*
evaluating previously unrequested arcs. Existing independent Dijkstra, topology,
blocking Marker, approach-cost and reset/replay checks remain enabled.

Suffix comparisons request just their arcs under one context. Explanations use
the same demand scorer, with a single traversal budget covering reverse search
and continuation reporting. Selected historical evidence remains separate from
current comparison evidence. Evaluated costs and cumulative search scores are
validated before frontier insertion.

`Graph::getRouteWorkCounts()` exposes cumulative decisions, prepared non-walking
arcs, evaluated arcs, cache hits, expanded vertices and preparation time.
Existing counters expose topology builds, lower-bound builds/hits, scratch growth
and bytes. Source-index `arcScoringSeconds` now measures relaxation/scoring time;
snapshot preparation has its own timer. Lower-bound construction remains a
separately counted whole-Graph operation on target-cache misses.

Run `--routing-scale-checks` for the measurements. One Windows/MSVC Release run
recorded the following 1,000-query batches (timings are not portable limits):

| Workload | Cold Paths/s | Warm Paths/s | Evaluated arcs/batch | Expanded vertices/batch | Prepared arcs/batch |
| --- | ---: | ---: | ---: | ---: | ---: |
| Nearby, 1,000-Marker Corridor | 1,876,880 | 2,807,410 | 2,000 | 1,000 | 0 |
| Distant, same Corridor | 14,404 | 14,383 | 1,088,000 | 544,000 | 0 |
| 1,000 distinct destinations | 23,750 | 23,581 | 544,598 | 272,299 | 0 |
| Mixed 1,000-Agent population | 16,195 | 16,229 | 837,381 | 415,618 | 56,000 |

The nearby query scores two arcs, not the whole Corridor. Warm scratch growth is
zero in all workloads, including target LRU eviction. Distinct destinations build
1,000 lower-bound tables per batch; nearby/distant warm batches hit 1,000 times.
The mixed population retains four shared target tables, 684,856 workspace bytes,
and digest `16756504350970399886` across cold/warm/reset/fresh-World runs. Snapshot
preparation took about 4.6 ms per warm population batch. The Door observation
fixture records cold/unchanged/changed throughput separately (about 1.17/1.18/1.17
million Paths/s here), with 3,000 evaluations, 2,000 expansions and 2,000 prepared
arcs each; changed observations refresh costs without changing work counts.

Release editor/headless builds and all 59 registered tests passed. Interactive
stress-World UI verification remains manual; the #224 generator is unchanged.

## Route planning time properties (#252)

Run `prometheum-fermide-headless.exe --route-planning-time-checks` (or CTest's
`route-planning-time-properties`) for headless coverage of Minimum and Maximum
route planning time. These Pathing properties default to 1 and 3 seconds and
accept finite values from 0.1 through 10. Tags author sampling ranges; individual
values override each sample independently. Effective maximum is at least the
effective minimum, without rewriting authored endpoints or sample provenance.

World schema 29 and Agent tag registry schema 13 persist these properties.
Earlier documents and clipboard objects without them retain the defaults.
Checks cover validation, crossed endpoints, exact sample round trips,
reconciliation, dependency conflicts, dirty state, individual and registry
undo/redo, and clipboard provenance.

## Initial runtime Route planning (#253)

Run `prometheum-fermide-headless.exe --route-planning-checks` (CTest:
`route-planning`) for stationary planning, inclusive upward-rounded tick
intervals, exact expiry, delayed arrival/Route loss, cancellation, pause,
deactivation, deterministic episode replay and Reset, persistence exclusion,
Selection panel text, editor-immediate Paths, and grey debug badge coverage.

Runtime Marker commands retain destination identity without calculating a Path.
Each entered episode samples an inclusive whole-tick duration from effective
Minimum/Maximum route planning time using an independent World-seed/Agent-ID
counter stream. NoOp commands do not consume episodes. Cleanup calculates the
Path synchronously after the last complete planning tick; physical movement
starts on the next tick. Pause and deactivation freeze the episode. API v1 reports `idle` during planning and v2 reports `route_planning`, while runtime snapshots expose the distinct
`RoutePlanning` state, destination Marker, and total/remaining ticks. The Agent
Selection panel converts those ticks to seconds. Editor Path authoring remains immediate. Voluntary route reconsideration now
uses the timed planning described under #256 below.

## Interruptible Route planning (#254)

The `route-planning` check also covers duplicate commands, fresh replacement
samples, boundary-published superseded/explicit cancellation outcomes, cleared
planning state, and Door queue/crossing interruptions without position snaps.
Committed crossings retain their transactions and defer sampling until commit;
arrival at the replacement Marker publishes `destination_reached` without a
planning episode. Uncommitted traversal ownership is released before planning.
The headless smoke suite exercises the Lua `superseded` status and cancellation
reasons through real behaviour callbacks as well as ordinary World commands.

## Mandatory automatic Route planning (#255)

Topology restoration and authorization loss/refusal enter timed Route planning
rather than calculating a replacement Path immediately. Committed movement
finishes at a safe boundary first; stale requests, permits, queue claims,
reservations and interaction ownership are released before planning. Rebuilding
again preserves the sampled episode and destination intent. At expiry, Marker
destinations resolve by identity, and non-Marker destinations resolve from the
retained Sector and local position against the current graph.

The `route-planning` and headless smoke checks cover repeated rebuilds, delayed
Marker removal and disconnected-route failure, assigned-idle fallback restoration,
Door queue/crossing cleanup, authorization changes, exactly-once Route loss, and
next-tick movement. Successful same-destination replanning publishes no movement
outcome. Failure clears intent and reports the existing Destination removed,
Topology changed, or Unreachable reason. Authored document Path restoration stays
immediate; pending runtime episodes are not serialized.

## Voluntary Route planning (#256)

Queue-delay and relevant authorization-gain triggers stop the Agent and privately
retain its Path and historical diagnostics. The active Path is empty during
planning; queue tickets, positions, permits and uncommitted interaction ownership
are forfeited. At expiry, current observations score the retained suffix and a
new Path together. Route persistence preserves the old Path unless replacement
improves enough. Invalidating the candidate upgrades the episode to mandatory
replanning without resampling; repeated environmental triggers do not extend it.
Movement resumes on the tick after expiry, or failure publishes Route loss.

The headless `route-planning` checks cover authorization gains, both persistence
outcomes, successful/failed candidate invalidation, repeated triggers, stationary
expiry and queue forfeiture. Existing transport-cycle and Ladder batch-fairness
fixtures disable voluntary reconsideration within their observation windows:
those assertions specifically require continuous queue membership. Default-policy
queue traces still run deterministically, though long-wait trace digests change.

## Route planning integration (#258)

The finalized workflow and compatibility contract are documented in
[Route planning](route-planning.md). `route-planning` additionally checks mixed
initial/voluntary/mandatory episodes over repeated Reset, property random-stream
noise, pause/resume and deactivation/reactivation; active Agent group membership
and `pf_agents_active`; and byte-identical World/clipboard output while planning,
including privately retained candidates. `smoke-behaviours` compares planning
samples with and without unrelated Lua random draws (select
`pf-smoke-behaviours --check planningIgnoresBehaviourRandomConsumption`).

The same `route-planning` target exercises the actual editor Agent renderer and
Selection panel headlessly: no OS window, graphics device, dialogs, or human input.
Command-stream assertions cover every planning tick, Agent Debug toggling, success,
failure, cancellation state exit, and the queue-to-planning badge stack transition.
Entering planning releases queues, so normal runtime states have one badge rather
than simultaneous queue/planning badges; the renderer independently stacks any
reported badges without a wall-clock cache. UI checks also cover effective-property
normalization in `route-planning-time-properties`. `render-checks` covers the wider
World canvas command stream. These automated interactive-surface checks are not a
claim of manual visual verification in a running editor.

Run both Debug and Release full builds and `ctest --output-on-failure` as described
below. The complete suite includes old/new document, tag registry, clipboard,
mixed-version behaviour reload, metrics, coordination and rendering regressions.
No test needs an interactive confirmation or message box.

Integration validation on Windows/MSVC: complete Debug and Release builds passed,
including the editor; all 67 CTest tests passed in each configuration (approximately
479 seconds Debug, 42 seconds Release). `git diff --check` passed. No manual editor
session was used.

## Ordinary Lift destination permissions (#260–#262)

After #290, build and run permission coverage independently:

```sh
cmake --build build-linux --target pf-smoke-permissions pf-smoke-permissions-editor --parallel
ctest --test-dir build-linux -R '^smoke-permissions' -j 3 --output-on-failure
```

Use `--list` and `--check <name>` on either executable for focused runs. The old
`--access-permission-checks` selection is retired pending compatibility dispatch;
it returns 2 with migration guidance. See [smoke modules](smoke-modules.md) for
ownership, dependency tiers, CLI contracts, and Linux validation.

`smoke-permissions` and `smoke-permissions-editor` cover the public World
queries and paused-only mutations, panel-commit undo/redo, YAML and binary round
trips, legacy unrestricted defaults, transactional malformed-data rejection,
rename/delete usage, and Stop retention through Lift and Location edits and
Sector reindexing. Deleted Stops and recreated Lifts start unrestricted.
Headless ImGui checks render the destination table, readable requirements, None,
and the selection-versus-accepted-journey explanation without a window or dialogs.

World schema 30 persists ordinary Lift requirements per destination Stop. #261
requires every listed Access permission for Agent-attributed selection commands,
using current direct and Permission set grants, including commands bound through
alternate Interaction points. Agentless commands bypass Agent authorization.
Routing tests cover rejection, missing-permission diagnostics, unrestricted
journeys past protected intermediate Stops, alternatives, Route loss, remote
observation exclusion, and completed locally observed piggyback journeys without
unauthorized selection. Under #269 those opportunistic ordinary Lift passengers
explicitly use Permission adherence false; default-true unauthorized passengers
decline the same accepted journey before boarding. Destination requirements do
not constrain disembarking or replace landing-call requirements.

Dynamic authorization checks cover direct and runtime grant loss, Permission set
assignment and membership changes, and requirement tightening before selection,
with alternatives and Route loss. Gains use voluntary planning and retain valid
Paths according to Route persistence. Shared passengers and the selecting Agent
complete their journeys and disembark after runtime revocation, paused authored
revocation, and requirement tightening. Pause/resume preserves onboard manifests
and accepted Stop requests during Route planning. Existing Reset, history,
persistence, lifecycle, and headless Selection checks remain enabled.
Shuttle and Platform lift destination requirements remain outside this milestone.

Validation for #262 on Windows/MSVC: full Debug and Release builds (including
the editor), all 67 non-GUI CTest tests in each configuration
(`ctest --test-dir build-windows -C <configuration> -LE gui --output-on-failure`),
and `git diff --check` passed. No manual editor session was used.

## Platform lift destination permissions (#263)

The Permissions modules run the shared Lift authorization scenarios with
Platform lift-specific Room and Walkway fixtures. Coverage includes all-required
direct/Permission set combinations, actual rejected and accepted selections,
missing-permission diagnostics, local piggyback journeys, remote observation
exclusion, protected intermediate Stops, grant loss and tightening, alternate
Paths/Route loss, voluntary gains and Route persistence, and Reset simulation.
Accepted shared journeys survive revocation and paused requirement edits.

The existing destination query, mutation, panel-commit and table APIs accept an
optional Room object index for Platform lifts. Under #270, destination piggyback
journeys now require explicit Permission adherence false when the passenger lacks
the destination requirement; the earlier #263 expectations are qualified rather
than changed retroactively. Mandatory ground and selected Walkway Stops use the
same per-destination requirements and schema-30 field as
ordinary Lifts; landing-call requirements remain independent. Tests cover YAML
and binary round trips, old-World defaults, malformed-input rollback, history,
rename/delete usage, unrelated settings and Sector reindexing, optional Stop
removal/recreation, supporting Walkway removal/recreation, and transport deletion.
The headless ImGui table verification requires no window, Agent debug overlay,
dialog, or manual input; no manual editor session is claimed.

Validation on Windows/MSVC: full Debug and Release builds including the editor,
all 67 non-GUI CTest tests in each configuration, focused permission checks, and
`git diff --check` passed. Shuttle destination permissions remain out of scope.

## Shuttle destination permissions (#264)

Shuttles reuse the destination requirement APIs, schema-30 field, panel commits,
usage reporting, and authorization rules established for Lifts. Under #271,
destination piggyback journeys require explicit Permission adherence false when
the passenger lacks the destination requirement; this qualifies #264's earlier
unconditional piggyback expectation without changing that parent issue. Historical
`LiftDestination` API names now cover all three transports; destination positions
are absolute x coordinates for Shuttles. Selection displays each Stop and its
position without Agent debug visibility. Requirements follow retained Stop
identity through vehicle edits, Stop reindexing, and supporting Location removal;
recreated Stops and transports start unrestricted.

The Permissions modules run the shared command, authoring, persistence/history,
and dynamic authorization checks with Shuttle fixtures. Two coupled Carriages
board from separate Locations and share one protected destination; unauthorized
passengers complete locally observed accepted journeys and disembark after
runtime or authored revocation and paused requirement tightening. Other checks
cover missing-permission diagnostics, partial direct/Permission set grants,
remote observation exclusion, a second boarding Stop, completed journeys past
protected intermediate Stops, walking alternatives/Route loss, voluntary gains
with Route persistence, and Reset restoring authored grants. Headless ImGui
verification checks Stop/x labels, summaries, None defaults, and the accepted
journey explanation. No OS windows, dialogs, or manual UI session are used.

Validation on Windows/MSVC: complete Debug and Release builds including the
editor, focused permission checks, all 67 non-GUI CTest tests in each
configuration, and `git diff --check` passed.

## Transport landing Permission adherence (#268)

`smoke-permissions` owns `smoke/permissions/LandingAdherence.cpp` for
Lift, Platform lift, and Shuttle landing requirements, without authoring any
destination Permission requirements. Effective Permission adherence true requires
landing authorization even when the vehicle is locally boardable; false retains
locally observed opportunities but never authorizes a protected call or assumes
future assistance. A different Level in the same Room is not a local landing
observation. Direct and Permission set grants combine with all-required semantics;
an unrestricted landing does not inherit another Stop's requirements.

Planning and runtime admission enforce the same willingness. Shuttle Door
reassignment also filters the selected landing. Already-granted crossings and
Platform lift virtual-boundary transfers finish safely; onboard continuations and
alighting are not new admissions. Existing grant/property/requirement replanning
conventions remain in use, including accepted shared Stop requests.

Tests cover inherited and individual values, the real Selection effective-property
display, registry undo/redo, loaded-profile journeys, unauthorized call rejection,
remote observations, stale Paths, alternative transport/walking Paths and Route
loss, pre-boarding grant loss and requirement tightening, changes during boarding
and riding, and safe exits for
both the changing passenger and another passenger sharing the journey. No OS
window, clipboard access, or manual editor interaction is required.

Landing authorization remains distinct from destination selection. Protected
destination Permission adherence integration belongs to the separate
transport-specific integration tickets, not this landing slice.

Validation on Windows/MSVC: complete Debug and Release builds including the
editor, all 67 non-GUI CTest tests in each configuration, and `git diff --check`
passed. No manual editor session was used.

## Ordinary Lift destination Permission adherence (#269)

The ordinary Lift destination scenarios now make opportunistic passengers
explicitly use Permission adherence false. An unauthorized Agent with effective
adherence true declines the same locally observed accepted Stop request before
boarding; effective direct and Permission set grants satisfy the all-required
destination requirement. Destination selection commands retain their independent
current-grant authorization checks for both adherence values.

Route capture and boarding admission both evaluate the current effective value,
grants, and shared destination requirement. Tests reinstall a stale Path after
adherence becomes true and require established Route loss, and change adherence
only after boarding to verify that the accepted request, ride, and safe exit
survive. Existing dynamic grant/requirement tests continue to cover alternatives,
no-route outcomes, accepted requests, and changes before and after boarding.
Landing-call requirements remain independent. #270 and #271 extend the destination
rule to Platform lifts and Shuttles respectively.

## Platform lift destination Permission adherence (#270)

Platform lift destination scenarios now follow the ordinary Lift integration:
an unauthorized Agent with effective Permission adherence true declines a
protected destination before boarding even when another Agent has already made
the shared Stop request. An explicitly non-adhering Agent may use that locally
observed journey but remains unable to issue the protected selection. Effective
direct and Permission set grants continue to use all-required semantics.

Route capture and open-platform admission independently evaluate the current
effective property, grants, and the World-owned destination requirement. The
admission check occurs before queueing and before the virtual-boundary transfer;
once that transfer is committed, or the Agent is already riding, requirement,
grant, and adherence changes do not cancel the accepted journey or block a safe
exit. Landing-call requirements, Mobility, capacity, queues, and local
observation remain independent.

The shared public World/simulation scenarios cover mandatory ground and optional
Walkway requirements, explicit true and false values, direct/set grants, real
selection commands, stale Paths and Route loss, alternatives, pre-boarding and
onboard changes, accepted requests, and completed exits. This qualifies #263's
unconditional piggyback expectation without changing that parent issue. #271
extends the same rule to Shuttles.

## Shuttle destination Permission adherence (#271)

Shuttle destination scenarios now require an unauthorized opportunistic passenger
to have effective Permission adherence false. An adhering unauthorized Agent
declines a protected destination before boarding even when another Agent has made
the shared Stop request; effective direct and Permission set grants satisfy the
all-required requirement. Neither adherence value authorizes the passenger's own
protected selection command, and landing-call requirements remain independent.

Route capture and boarding admission evaluate the current effective property,
grants, and shared destination requirement. Admission resolves the Shuttle journey
resource so coupled Carriages, distinct Stop positions, Door reassignment, and all
boarding origins use one requirement. Once boarding commits, later adherence,
grant, or requirement changes do not cancel the accepted request or prevent safe
disembarkation. Mobility, capacity, queues, safety, and local-observation rules
remain unchanged.

The shared public World/simulation scenarios cover explicit true and false values,
direct/set grants, real selection commands, stale Paths and Route loss,
alternative and no-route outcomes, changes before and after boarding, accepted
requests, completed exits, two coupled Carriages, and journeys from multiple
origins. This qualifies #264's unconditional piggyback expectation without
changing that parent issue.

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

## Reset and reload profiling

Run `prometheum-fermide-headless.exe --restoration-benchmark <world>` for five
load/run/reset/run/release cycles with authored-state, Path, trace and lifetime
checks. Set `PF_RESTORATION_TIMING=1` for nested phase timings. This measures
restoration separately from routing throughput and fixture generation; timings
are informational. See [reset/reload measurements and reproduction](restoration-performance.md)
for the binary-versus-YAML experiment, root cause, before/after distributions,
memory observations and remaining manual editor check.

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
