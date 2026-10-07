# Door clearance: Pose, support, live changes and automatic Crawling (#476–#477, #483–#485)

Ordinary Regular and Tall Doors share one top-relative clearance rule. The top
above the approach Floor includes effective Height modifiers, Sitting's and
Crouching's 0.6 body-height scale, Crawling's 0.3 body-height scale, or
Lying's rotated vertical extent (body width). Crawling uses its reduced height
rather than Lying's body width. Physical
support elevation is added to that extent. Exact fits are accepted with a
`0.00001` world-unit tolerance. Standalone Bulkhead Doors use the same rule
against their physical opening. Chamber and transport-owned thresholds keep
their existing rules; admitted crossings may finish safely.

## Automatic low-Door Crawling (#483–#484)

An Agent completing a Marker journey through a manual, automatic or
remote-controlled ordinary Regular or Tall
Door crosses **Standing** when Standing fits, otherwise **Crawling** when that
fits, and is refused when neither envelope fits. The classification is one
shared seam — `Door::classifyAgentCrossing` — used by route feasibility,
route-cost facts and captured inputs, live Path validation, request/queue gates
and permit adoption; `admitsAgentTraversal` is its `!= None` wrapper.

- The Agent waits Standing while the Door opens and in any queue. On an admitted
  low crossing it switches directly to Crawling (30% of effective Standing
  height), crosses at **50%** of its ordinary threshold-crossing speed (the
  in-place Door crossing doubles from six to twelve 1/60-second ticks), then
  stands again immediately after full completion. There is no Crouching stage
  and no preparation/recovery delay.
- A retained lowered Action Pose (Sitting/Crouching/Crawling/Lying set by Lua at
  the threshold) keeps its own envelope and never triggers the automatic
  Crawling fallback; an Agent that is effectively Standing (or predicts a
  Standing departure, including Furniture use) may crawl.
- Objective crossing facts and captured route inputs record the doubled motion
  duration, so real competing routes reflect the slower threshold crossing in
  perceived cost. Doors too low even for Crawling keep normal alternative
  routing or Route loss; permissions, Mobility, queues, lanes, permits, Broken
  behaviour and interlocks remain authoritative.
- Pause/deactivation freeze the admitted crossing and its Crawling Pose; an
  already admitted crossing completes safely after live Height/envelope changes.
  Reset restores Standing, and repeated low Doors each perform their own
  crossing-scoped Crawling-to-Standing cycle.

### Automatic and remote-controlled opening (#484)

All three activation modes use the existing shared clearance, cost and permit
adoption path; no separate low-Door activation protocol is needed. Automatic
sensing and remote physical control operation still open the Door normally.
Opening, control-approach and queue waits remain Standing, even when activation
is delayed or fails. Crawling begins only on adopted crossing admission.
Remote Interaction points retain their Access permission requirements and
Buttons Mobility restrictions. Unavailable or Broken closed Doors remain
unusable; no partial-opening passage is introduced. Opening styles, physical
width, crossing lanes and fair queue order are unchanged. Tall Doors cannot
author a Height scale and fit all supported unsupported Standing Heights.

Public-World regressions cover every opening style and activation mode in both
directions, Standing/low/exact/tolerance/impossible fits, alternatives and Tall
Standing journeys. Controlled low-Door journeys additionally cover protected
controls with and without grants, forbidden Buttons/Door Mobility, Broken and
Unavailable operation, queued followers, pause/deactivation, cancellation and
replacement before/after admission, live Height enlargement and Reset. Direct
and captured cost regressions verify six-tick Standing versus twelve-tick
Crawling motion without revealing unobserved opening state. A slightly shorter
open low-Door route loses to an open Standing alternative because of that
extra motion cost; restoring Standing clearance restores the shorter choice.

#484 validation: incremental Release build including `editor` passed; all
**70/70 repository CTests** passed (unmodified Willpower submodule tests
excluded). The shared implementation from #483 already supported these modes;
#484 delivers the mode-specific regression coverage and documentation without
adding duplicate runtime policy.

### Standalone same-Layer Bulkhead Doors (#485)

Standalone Bulkhead Doors now use the same Standing/Crawling/impossible
classification in direct and captured route facts, remaining-Path validation,
request/queue gates and permit adoption. Clearance uses the physical doorway
top relative to the approach Floor; this adds **no Height scale authoring**.
The standard 0.7-unit opening fits all currently validated unsupported Agent
Heights. Reduced physical test fixtures use existing public Shape value
assignment, not private state injection or a new authoring/test-only command.

The crossing is horizontal, not the six/twelve-tick in-place ordinary Door
crossing. Its far-side vertex is a mandatory physical exit boundary, 0.3 units
beyond the wall centre: Door half-thickness plus Agent half-width. The Agent
waits Standing, crawls only after admission, moves at half normal speed until
fully across, then immediately stands. Horizontal position updates retain the
admitted Pose rather than applying ordinary walking's Standing reset. Route
motion costs double the corresponding threshold walking duration.

A fully open Bulkhead remains an unconstrained bidirectional passage, with no
new crossing-lane restriction. Manual, automatic and remote activation retain
their existing operation rules; protected controls and Mobility remain binding.
Broken-open passage remains usable; Broken-closed or unavailable operation
does not gain partial-opening passage. Admitted crossings retain their permit,
position and Pose through pause/deactivation and complete safely after
cancellation, replacement or a live envelope change. Reset clears the task and
restores Standing. Chamber/Airlock journeys are not changed by this slice.

`bulkheadCrawlingJourneys`, `bulkheadCrawlingLifecycle` and
`bulkheadCrawlingGates` exercise real World Marker journeys in both directions,
repeated thresholds, exact/tolerance/impossible fits, physical exit and
half-speed motion, direct/captured cost agreement, and alternative routes.
A direct Standing route wins over an adjacent-Layer bypass; doubling only the
Bulkhead motion cost makes that bypass win. Identical Broken-open journeys
verify doubled physical crossing duration. Lifecycle checks cover frozen
pause/deactivation, before/after-admission cancellation and replacement, live
Height and geometry changes, Broken changes, Reset and terminal cleanup.
Protected control, unavailable, forbidden Mobility and Broken-open/closed
checks ensure Crawling does not bypass operation or feasibility rules.

#485 validation: the incremental Release build (including `editor`) passed;
all **70/70 repository CTests** passed, with unmodified Willpower submodule
tests excluded. The smoke ownership manifest includes the new Agent fixture.

`Agent::getDoorClearanceExtent()` describes the **current** envelope.
`getTraversalDoorClearanceExtent(beginningMovement)` predicts departure:

- A new ordinary movement request uses Standing without Furniture support.
- Definition-owned Furniture use also predicts Standing at the admission gate,
  because departure finishes use even though planning preserves it.
- An already-positioned Agent retaining a one-shot Action Pose at a threshold
  uses its actual envelope at admission. Beginning another ordinary journey
  still restores Standing; this does not add seated locomotion or movable
  Furniture.

Direct route facts and captured route inputs use the same calculation. Ordinary
route contexts default to `beginningMovement = true`; a diagnostic evaluation
of an already-positioned retained-Pose crossing explicitly uses `false`.
Runtime checks repeat the predicted envelope at request, queue and permit
adoption boundaries, never interrupting an admitted crossing.

## Shared clearance decision boundary

Routing and every admission gate route their fit/refusal question through one
seam: `Door::admitsAgentTraversal(agent, approachFloorY, beginningMovement)`.
It combines the Agent's effective traversal envelope with the Door's top
clearance above the given approach floor and preserves the Door's scope
(ordinary Regular/Tall Doors between Locations restrict clearance; specialized,
transport-owned and Chamber thresholds keep their existing rules). Callers
retain their own approach-floor source and `beginningMovement` value; future
crossing modes build on this single boundary instead of duplicating the fit
question at each gate.

## Physical support versus decoration

A Lua Furniture usable point may declare `supportElevation`, a finite,
non-negative height above its supporting Floor/Walkway; omission means zero.
It applies while the Agent claims that point. It is separate from the point's
Floor-level routing position, placement Level, artwork tile offsets and
`getPoseRenderXOffset()` / `getPoseRenderYOffset()`. In particular the existing
claimed-Lying presentation adjustments (-0.25 X, +0.25 Y) are decorative and
are **not** inferred to be physical support. A catalogue must declare any real
support separately. This metadata does not enable moving Furniture or claim
that an Agent will retain its support during an ordinary new journey.

Example:

```lua
usablePoints = {{
  key = 'seat', label = 'Seat', x = 0.5,
  supportElevation = 0.03,
}}
```

Support metadata is retained in the immutable external Lua definition, not in
persisted Agent Pose or new runtime movement state. Negative, non-numeric and
non-finite values are rejected while loading the catalogue.

## Reproducible Release diagnostic

Fixture: `src/headless/smoke/agent/DoorClearance.cpp`.

```sh
cmake --build build-linux --target pf-smoke-agent -j 4
ctest --test-dir build-linux -R '^door-pose-clearance-' -V
```

Use a configured **Release** build tree. The runner prints labelled
`[clearance]` observations from `poseDoorClearanceDiagnostics` and
`poseDoorMovementReset`. These dedicated CTests enable
`PF_DOOR_CLEARANCE_TRACE=1`; ordinary smoke and contract invocations preserve
their standard CLI output. Existing Standing regressions are also available via
`ctest --test-dir build-linux -R '^smoke-agent$' --output-on-failure`.
Generated Lua packages live in the smoke harness's temporary fixture directory.
No editor interaction or private-field editing is required.

### Retained-Pose diagnostic boundary

Current editor workflows do not request seated or lying locomotion. To cover
that future envelope without introducing such mechanics, the fixture:

1. Creates real adjacent Rooms at Level 1, an ordinary Door, an Agent and
   catalogue-backed Furniture with a usable point at the Door approach.
2. Uses an explicit Lua Action to pose/claim through the existing World workflow.
3. Selects an actual two-node Agent Path while already positioned at the
   threshold. Before the next intent-collection phase, a narrowly scoped
   diagnostic driver dispatches the same validated authored Action there. It calls the production World Action execution seam;
   it does not write private Agent state or replace clearance/queue logic.
4. Evaluates direct and captured retained-Pose traversal facts, then advances
   actual fixed-timestep Agent/coordinator simulation. Fitting cases must be
   observed in `TraversingEdge` with the retained Pose and arrive in the other
   Room. The ordinary Layer-transfer commit subsequently restores Standing.
5. Refused cases must enter Route planning with no crossing permit. The fixture
   ends that retained-envelope episode at refusal: a new ordinary journey would
   reset Pose/support, potentially fitting a Tall Door even when the original
   raised-support envelope did not. It does not pretend that an ordinary retry
   retains Furniture or report this refusal as an artificial Route loss.

The labelled cases cover:

- Sitting and Crouching: floor-supported fit (.27 body under .28 opening),
  raised refusal, exact fit, within-tolerance fit and over-tolerance refusal.
- Crawling: .135 body, proving the 30% height is used rather than Lying's .40
  width (a .20 opening admits Crawling, a .13 opening refuses), plus raised
  refusal and tolerance boundary.
- Lying: .40 rotated extent, fit/refusal with support and tolerance boundary.
- Effective Height modifier .7, including Sitting/Crouching exact fit/refusal,
  Crawling scaling with the modifier, and Lying width remaining unchanged by a
  modifier of the upright long dimension.
- Both directions, nonzero approach Level, and ordinary Tall fit/refusal.
- Paired Lying cases differing only in the decorative offsets (unclaimed
  versus zero-elevation claimed point), both fitting and refusing.
- Invalid support metadata.

`poseDoorMovementReset` uses normal explicit **Use furniture** and public World
movement requests for Sitting and Lying. With no alternative, it observes Route
loss and preserved use/occupancy. With a feasible alternative Door, it observes
Standing, arrival, released occupancy, and no permit through the low Door.
These cases exercise the ordinary retry/Route-loss lifecycle separately from
the retained-envelope diagnostic.

## Live-change safety (#477)

Remaining ordinary Door edges are checked against the effective traversal
envelope before each active Agent collects traversal intent. This catches
geometry, individual/tag Height, Pose and physical support changes even when
neither topology nor remembered device condition changes. Infeasible suffixes
use existing Route planning/Route loss; a sampled planning interval is not
restarted. Queued/stale Paths still face the independent request, queue and
permit-adoption clearance gates. Fitting queues, permissions and Mobility rules
remain unchanged.

An already admitted edge is excluded from invalidation. A plain pause freezes
an ordinary Door crossing in place with its commitment intact; resume completes
it even if its Agent's envelope has grown. World Height scale and Regular/Tall
edits refuse active crossings (including paused crossings) before authored state,
geometry, dirty state or history can change. Running World height edits also
refuse before mutation. Idle accepted edits remain undoable.

### Release live-change diagnostics

```sh
cmake --build build-linux --target pf-smoke-agent pf-smoke-editor -j 4
ctest --test-dir build-linux -R '^door-(live|pose)-clearance-' -V
```

The new labelled `[live-clearance]` observations are:

- `door-live-clearance-diagnostic`: 288 World journeys, changing Door scale
  (`change=0`), individual Height (`1`) or tag-supplied Height (`2`) after route
  selection, near/queued and far from admission. Both directions, all four
  opening styles, all three activation modes and alternative/no-alternative
  outcomes are covered. Authored changes use the supported paused editor World
  seams; a plain pause already replans waiting movement.
- `door-live-clearance-committed`: six Regular/Tall journeys in both directions.
  The existing validated Action diagnostic changes retained Pose (`admitted-change=0`)
  or physical support (`1` Regular, `2` Tall) after admission. All complete;
  a subsequent stale oversized Path is refused with Route planning and no permit.
  This uses the #476 Action dispatch seam, not private-field manipulation or
  new seated locomotion.
- `door-live-clearance-editor`: real document snapshots and history check running
  and paused edit refusal, unchanged YAML/topology/dirty state/history, preserved
  crossing position, individual Height enlargement during crossing, safe arrival,
  and undo/redo of the preceding accepted scale edit with restored clearance.

The existing Pose diagnostics change the envelope on selected external Paths
before intent collection, observing fitting crossings or Route planning without
oversized permits. `standingDoorWorldJourneys` also covers a stale oversized queue
head with a fitting follower. The combined Agent, World and Editor smoke modules
retain the Location-pairing, Tall/excluded-ownership and legacy-geometry matrix.

## Verification record

#483: the complete incremental Release build (including the editor) succeeded;
all **115/115 CTests** passed, including the new `automaticCrawlingJourneys`
check (Standing fit, Crawling fit with the doubled twelve-tick crossing, exact
Crawling fit, refusal of both, both directions and the optional alternative
Door). Standing-only assertions in `standingDoorClearance`,
`standingDoorWorldJourneys`, `liveDoorClearance` and `poseDoorMovementReset`
were updated for the automatic Crawling fallback. `git diff --check` is clean.
Interactive editor checks were not run; the labelled World diagnostics are the
automated counterpart for the manual workflow.

#477: the complete incremental Release build (including the editor) succeeded;
all **115/115 CTests** passed. The five combined dedicated diagnostics also
passed. `git diff --check` is clean. Interactive editor checks were not run;
the document/history fixture and labelled World diagnostics are the automated
counterparts for the ticket's manual workflow.

### Earlier #476 verification

Release `smoke-agent`: all 67 checks passed, including both new checks and the
existing Standing clearance and journey regressions. The complete incremental
Release build succeeded and the full Release CTest suite passed **112/112**.
The dedicated diagnostic invocation also passed and printed **50 labelled
observations** covering crossings/refusals, invalid supports and movement resets.
The retained fixture logs physical support, decorative Y offset, effective
modifier, envelope, opening, route feasibility and crossing/refusal outcome
for each case.

No interactive editor observations were performed in this implementation
session. Manual chair/bed departure verification remains available using the
workflow in ticket #476; the public-World movement-reset tests exercise its
simulation and Furniture-use semantics headlessly.
