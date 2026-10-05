# Explicit Marker movement Actions (#457)

This integration-branch slice exposes movement through
`World::moveAgentToNamedMarker(agent, name, action)` and
`World::moveAgentToMarker(agent, marker, action)`. The default is the immutable
built-in `core::IdleAction` identity (`idle`, displayed as **Idle**).
`World::availableAgentActions(marker)` returns Idle only; additional Actions,
Lua registries and Furniture use/finish callbacks belong to subsequent slices.
Unknown Actions are refused, never silently replaced with Idle.

Names resolve once at acceptance to World-owned Marker identities. Rename and
supported topology reconstruction retain the target. Deletion cancels it;
reusing its name cannot redirect accepted intent. A repeated pending request to
the same Marker and Action is a NoOp. A replacement publishes cancellation of
the old request, and a request to an already reached Marker still completes at a
simulation boundary, not synchronously at acceptance.

Agent snapshots and movement outcomes expose `selectedAction`.
`DestinationReached` means successful physical arrival; `RouteLost` reports route
failure; `MovementCancelled` distinguishes explicit cancellation, replacement
(`Superseded`) and deletion (`TargetDeleted`). Committed crossings and occupied
transport journeys retain their existing safe-exit rules. Outcomes remain
available through the public event queue independently of behaviour observation.

Idle invokes no Furniture effects, claims no usable point, schedules nothing,
and leaves an assigned behaviour enabled. Legacy catalogue Sit/Lying fields no
longer cause arrival or intermediate-passage effects. The selected-Agent editor
panel visibly offers Idle in its movement Action selector, including when
choosing a destination Marker.

World schema 53 records saved Path Action intent as `action: idle` in YAML and
binary documents. Missing Action fields resolve to Idle; unavailable values are
rejected. Reconstruction does not persist or execute Lua state. The existing
reset/paused/history Path-intent rules remain in force.

## Verification and migration boundary

Public World/document checks cover default and explicit Idle, planning and
physical arrival timing, same-target requests, replacement, rename, deletion,
name reuse (including a separately issued new request), absent/invalid document
Action fields, and behaviour ownership after Idle arrival. Supporting CPU-only
editor coverage exercises the production selector and planning presentation.
All new checks remain in their existing domain-owned smoke modules.

Per #457, legacy implicit-use fixtures are deliberately not migrated in this
slice. Full-suite green belongs to the final integrate-and-verify ticket. The
following legacy checks still require migration to explicit Furniture use:

- Routing: `furniture/seatRouting`.
- Simulation: `furniture/actions`, `furniture/bedLyingLifecycle`,
  `furniture/authoredChairArrival`, `furniture/seatedEdits`,
  `furniture/occupancyLifecycle`.
- Render: `furniture/bedRenderOffset`.

They are retained unchanged rather than bypassed, skipped, or satisfied by a
compatibility exception. Their owning functional and exhaustive-contract CTests
therefore report failures until the follow-up lifecycle/fixture work lands.

Final Linux validation built the default core/headless/editor inventory in both
Release and Debug, then ran the supervised unfiltered final CTest lane. Each
configuration had 110 CTests: 103 passed, one optional GUI capability test skipped,
and six failed solely because they own the seven legacy checks above. All new
and affected non-legacy coverage passed, and `git diff --check` was clean.
Retained final CTest evidence: Release run
`7844c529619845a2ba91b4a0df491de8`; Debug run
`d8d9e78f66734598b1d3629f412874f7`.
