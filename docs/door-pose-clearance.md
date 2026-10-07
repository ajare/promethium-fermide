# Door clearance: Pose and support (#476)

Ordinary Regular and Tall Doors share one top-relative clearance rule. The top
above the approach Floor includes effective Height modifiers, Sitting's 0.6
body-height scale, or Lying's rotated vertical extent (body width). Physical
support elevation is added to that extent. Exact fits are accepted with a
`0.00001` world-unit tolerance. Specialized and transport-owned thresholds keep
their existing rules; admitted crossings may finish safely.

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
3. Starts an actual two-node Agent Path and waits for its threshold boundary.
   A narrowly scoped diagnostic driver then dispatches the same validated
   authored Action there. It calls the production World Action execution seam;
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

- Sitting: floor-supported fit (.27 body under .28 opening), raised refusal,
  exact fit, within-tolerance fit and over-tolerance refusal.
- Lying: .40 rotated extent, fit/refusal with support and tolerance boundary.
- Effective Height modifier .7, including Sitting exact fit/refusal and Lying
  width remaining unchanged by a modifier of the upright long dimension.
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

## Verification record

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
