# Scripted Marker Actions (#457–#460)

This integration-branch slice exposes movement through
`World::moveAgentToNamedMarker(agent, name, action)` and
`World::moveAgentToMarker(agent, marker, action)`. The default is the immutable
built-in `core::IdleAction` identity (`idle`, displayed as **Idle**).
`World::availableAgentActions(marker)` returns Idle followed by the Marker's
ordered additional Lua Actions. Unknown or unassigned Actions are refused, never
silently replaced with Idle. Furniture use/finish callbacks remain deferred.

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
panel visibly offers Idle and loaded custom Actions in its movement Action
selector. The chosen destination must offer the selected Action.

World schema 54 records saved Path Action intent, registry basename/UUID and
ordered Marker assignments in YAML and binary documents. Schema 53 introduced
`action: idle`. Missing Action fields resolve to Idle; unavailable values are
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


## Lua registry contract (#458)

An external `example.actions.lua` returns an ordered registry:

```lua
return {
  api_version = 1,
  uuid = "ad603358-5ebf-45bb-a686-c3f491152c61",
  actions = {
    {
      key = "greet",
      name = "Greet",
      run = function(agent, world, marker)
        world.log(agent.name .. " arrived at " .. marker.name)
      end
    }
  }
}
```

Stable references are `registry-uuid:key`, never display names. Keys contain
ASCII letters, digits, underscores or hyphens (1–128 bytes); display names are
1–128 bytes. Duplicate keys/names, invalid UUIDs, array holes, missing/non-Lua
functions and captured module upvalues are refused. Define one-shot callbacks
without captured state; each invocation evaluates the accepted source in a fresh
sandbox. Built-in `idle` / **Idle** and `use-furniture` / **Use furniture** are
reserved; Use furniture is not yet available.

Read-only userdata views expose `agent.id/name/x/y`, `world.name/tick/api_version`
and `marker.id/name`. `world.log(message)` stages Info logging. No mutable domain
objects, graph, filesystem/process, debug, coroutine, wall-time or random APIs
are exposed. The immutable `prometheum.actions.v1` import identifies the host
contract; all other imports are refused. Source is capped at 256 KiB, registry size at
256 Actions, invocation memory at 2 MiB and execution at 100,000 instructions.
Caught budget exhaustion remains terminal. Logs are limited to 32 messages,
1 KiB each and 8 KiB total, with one suppression warning. Callback failure
publishes no staged logs, emits `ActionFailed` with `scriptFailure` and a bounded
diagnostic, pauses simulation and makes headless `advanceTick()` return false.

### Authoring, history and documents

Pause the World, select a Marker and enter the external `.actions.lua` path in
its **Agent Actions** panel; **Load Action registry** validates before attachment.
Use **Add Action**, **Up**, **Down** and **Remove** to author its ordered additional
Actions. Select the request's Action in the selected-Agent panel, then choose its
named destination Marker through the existing path-destination workflow. Idle
remains the default. Registry removal clears assignments; Undo/Redo restores
registry metadata/source dependencies, assignment order and authored request
identity without executing callbacks during reconstruction.

The public seams are `selectActionRegistry`, `setMarkerActions`,
`availableAgentActions`, `moveAgentToNamedMarker` / `moveAgentToMarker`, and the
paused document-authoring `authorAgentMarkerRequest`. Runtime requests remain
transient; authored requests persist their Action. Put the registry beside the
saved World: documents reference only its basename and expected UUID. Reopen
validates the registry and every assigned identity; a display-name change does
not invalidate stable keys. YAML/binary serialization contains no Lua source,
closures or VM state. History holds immutable package dependencies, not a VM.
Selecting the same attached reference does not reload it; live reload is #464.

Availability is checked at acceptance and arrival. Assignment/package removal
cancels pending requests at the next safe simulation boundary, preserving existing
safe-exit handling, with `ActionUnavailable` and a diagnostic. It never substitutes
Idle. Arrival invokes only the selected callback in stable Agent order; intermediate
passage does not execute it. Agent behaviour Action selection/outcome additions are implemented in #460 below.

Focused public checks: Simulation `markerActions/registry`, `/execution`,
`/failures`, `/documents`, `/logging`; Editor `markerActions/workflow`. They cover
real Lua logging, physical timing, deterministic independent Worlds/Agents,
assignment order/deduplication, reference failures, protected budgets and rollback,
registry/display-name/Marker identity, binary/YAML reopen and document history.


### #458 final Linux verification

The final source state built the complete default core/headless/editor inventory
in Release and Debug (`PF_HIGH_ANALYSIS=ON` for Debug). The supervised unfiltered
final CTest lane ran 110 tests per configuration: **103 passed, one optional GUI
capability skipped, six failed** solely for the seven unchanged legacy
implicit-Furniture-use checks listed above. All new Marker Action checks, Editor
functional/exhaustive/CLI coverage, persistence, sandbox coverage, ownership audit
and headless tools contracts passed. The restoration-benchmark timeout discovered
during development was repaired by avoiding new Action-goal reconstruction for
legacy Idle paths; final Debug tools coverage passed in 120.94 seconds.

Final build evidence: Release `0e6a28e9bff34baaa8e9a75de3f4cfdb`, Debug
`b540a31daa8449809b09a292792f9d86`. Final unfiltered CTest evidence: Release
`1956c91e2509487192f0f63380d4b11b`, Debug
`2391290974794030bc27b8c6a724ecb4`. `git diff --check` passed. No Windows
validation is claimed; full-suite green remains the #467 integration contract,
not a claim of this slice.

## Validated atomic effects (#459)

The version-1 World view adds these dot-call capabilities:

```lua
world.set_pose("sitting") -- standing, sitting or lying
world.claim()             -- selected Furniture-owned usable-point Marker only
world.release()           -- selected point, owned by this Agent only
world.request_device(12, "set-sector-lights") -- World Interaction point ID and type
```

Device requests invoke the existing point's authored typed bindings, not arbitrary
raw device commands. Supported types are `set-sector-lights`, `open-door`,
`set-extended-state`, `call-lift`, `select-lift-destination`, `call-shuttle`,
`select-shuttle-destination`, `request-airlock`, `set-booth-window-state`,
`toggle-booth-window`, `press-dumbwaiter-landing`, and `set-access-panel-state`.
All bindings on the selected Interaction point must match the requested type.
Scripts cannot change the binding's target, desired state or destination Stop.
At most one distinct device Interaction point is admitted per invocation, matching
the existing one-pending-interaction-per-Agent contract; duplicate requests coalesce.

An invocation stages up to 64 effects plus bounded logs. After the callback returns,
World validates the batch against shadow Pose/claim values, selected-point ownership,
current activation, Mobility, physical reach, Location, point eligibility, Access
permissions, transport destination permissions and pending-interaction authority.
No Pose, claim, request, operation event or script log is published on rejection.
Committed requests retain existing asynchronous device outcome and traversal gates;
operation acceptance does not grant a traversal permit or guarantee device success.

Occupancy/eligibility refusal emits `ActionFailed` with a diagnostic and
`scriptFailure == None`; it neither pauses nor fails headless advancement.
Existing occupied-destination routing may instead report `RouteLost` before the
callback can run. Lua exceptions or heap/instruction exhaustion discard the batch
and retain the diagnostic/pause/headless-failure policy. Only host-owned claims
are authoritative, scoped to one Agent and one Furniture-owned Marker. Source is
immutable, captured module state remains forbidden and each invocation still runs
in a fresh sandbox, including independent Furniture instances and Worlds.

Focused public-seam verification: `markerActions/atomicEffects`,
`markerActions/claimCompetition`, and `markerActions/deviceEffects`. These exercise
committed Pose/claim/release, deterministic competitors, independent instances and
Worlds, non-owner release refusal, pose-before-claim rollback, ordinary refusal,
throwing/heap/instruction failures, log/request rollback, permission/Mobility/reach
and typed-binding refusal, and observable asynchronous lighting completion.
Existing registry/containment/determinism checks are retained. Furniture catalogue
conversion and use/finish lifecycle, live reload and behaviour API work are not
part of this slice.

## Behaviour-selected Actions (#460)

The additive `prometheum.v2` movement contract is
`context.move_to(marker, action)`; `marker` is an opaque configured Marker or a
named Marker string resolved once at acceptance. Omitted/nil Actions are Idle;
v1 retains its opaque-Marker/Idle movement contract. Inspection and execution
both use the same World/coordinator availability validation as editor requests.
Same Marker plus same Action is NoOp; changing the Action is replacement.

Authored `action` configuration fields use display-name selection of stable
references. Defaults, nested values, history, clipboard and YAML/binary documents
retain the typed identity, never Lua state. Invalid references reject assignment;
registry selection/removal cannot invalidate authored behaviour Action fields.
The generated picker always offers Idle and offers loaded custom Actions when at
least one World Marker offers them. Request admission checks the chosen Marker.

Immutable behaviour outcomes include destination identity, Action identity,
result and event sequence/tick. Success is `destination_reached`, failure is
`action_failed` with `refused`/`script_error`, a diagnostic and semantic script
failure classification. Cancellation preserves `explicit`/`superseded` and adds
`target_deleted`/`action_unavailable`. Route loss retains its callback with an
additive fourth immutable outcome argument. See [behaviour packages](agent-behaviour-packages.md)
for the complete API and example. Idle neither schedules replacement activity
nor disables the assigned behaviour. Timers, stable delivery order and committed
traversal handling remain unchanged; script exceptions still pause/headless-fail,
with their queued outcome observed after public resume.

Public checks: Behaviours `scriptedActionOutcomes`, `scriptedActionScriptFailure`,
`scriptedActionCancellations`, plus extended v1/v2 Route-loss coverage; Editor
`markerActions/behaviourConfiguration`. They exercise custom completion and next
requests, ordinary refusal, exceptions/resume, omitted Idle followed by another
custom request, explicit/same-Marker replacement cancellation, Action removal,
target deletion, schema package loading, assignment refusal, generated controls,
Undo/Redo and YAML/binary reopen. State-changing Furniture demonstrations and
occupancy-conflict integration remain deferred to the Furniture lifecycle and
fixture-conversion slices, not implemented here.

### #459 final Linux verification

The final source state built the complete default core/headless/editor inventory
in Release and Debug through the supported supervised `--lane final --build-only
all` procedure. Unfiltered final CTest ran 110 tests in each configuration:
**103 passed, one optional GUI capability skipped, six failed**, exclusively for
the seven unchanged legacy implicit-Furniture-use checks listed above. All new
Action checks, containment/determinism, permissions, asynchronous interaction,
editor/persistence, headless tools and ownership/CLI coverage passed. Full-suite
green remains the #467 integration contract. `git diff --check` passed; no Windows
validation is claimed.

Final build evidence: Release `289dbdc89b2d4d129c3b16dd101fd79c`, Debug
`7dd3b38c83cb475380154508f63effe0`. Final unfiltered CTest evidence: Release
`892a9091442c4f9aa074706a6dc22090`, Debug
`bcdbea2cf3654aeca6982a10a15bd37c`.

### #460 final Linux verification

The final source state built the default core/headless/editor inventory through
supervised `--lane final --build-only all` in Release and Debug. Unfiltered final
CTest ran 110 tests per configuration: **103 passed, one optional GUI capability
skipped, six failed** exclusively for the seven unchanged legacy implicit-Furniture
checks listed above. Behaviour and Editor functional/exhaustive/CLI coverage,
persistence, Action checks, ownership and tools contracts passed; no new failure
was introduced. Full-suite green remains the #467 contract. `git diff --check`
passed. No Windows validation is claimed.

Final build evidence: Release `f4b84d4e7dec48fb89b861bd2dd12a15`, Debug
`9a6c66c3731f448f9dc79a68d5916011`. Final unfiltered CTest evidence: Release
`786d8f6e8cab461a92b7cd2bc08b8294`, Debug
`a8a0624e24c648f6bd272b696b3a1ea5`.
