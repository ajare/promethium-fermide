# Scripted Marker Actions (#457–#467)

The integrated feature uses only Lua Furniture catalogues and explicit Actions.
Bundled and regression dependencies are converted; the YAML loader and implicit
arrival adapters are removed. Validation sections below record individual slices'
**historical** results and deferred work, not current failures or remaining tasks.
See [final integration evidence](scripted-actions-integration.md) for #467.

The public movement workflow is
`World::moveAgentToNamedMarker(agent, name, action)` and
`World::moveAgentToMarker(agent, marker, action)`. The default is the immutable
built-in `core::IdleAction` identity (`idle`, displayed as **Idle**).
`World::availableAgentActions(marker)` returns Idle, derived Use furniture when
the owning definition provides paired callbacks, then the Marker's ordered
additional Lua Actions, without duplicates. Unknown or unavailable Actions are
refused, never silently replaced with Idle.

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

Idle claims no usable point, schedules no new activity, and leaves an assigned
behaviour enabled. Replacing active Furniture use with Idle finishes that use.
Legacy catalogue Sit/Lying fields and their arrival machinery are removed. The selected-Agent editor
panel visibly offers Idle, derived Use furniture and loaded custom Actions in
its movement Action selector. The chosen destination must offer the selected Action.

World schema 54 records saved Path Action intent, registry basename/UUID and
ordered Marker assignments in YAML and binary documents. Schema 53 introduced
`action: idle`. Missing Action fields resolve to Idle; unavailable values are
rejected. Reconstruction does not persist or execute Lua state. The existing
reset/paused/history Path-intent rules remain in force.

## Historical #457 verification and migration boundary

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
therefore report failures until the follow-up fixture migration lands.

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
are exposed. The immutable `promethium.actions.v1` import identifies the host
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
Selecting the same attached reference does not reload it; use the explicit paused
Reload Action registry workflow described under #464 below.

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
world.set_pose("sitting") -- standing, sitting, lying, crouching or crawling
world.claim()             -- selected Furniture-owned usable-point Marker only
world.release()           -- selected point, owned by this Agent only
world.request_device(12, "set-sector-lights") -- World Interaction point ID and type
```

`set_pose` accepts the runtime pose vocabulary `standing`, `sitting`, `lying`,
`crouching` and `crawling`. Crouching's bodily height is 60% and Crawling's is
30% of effective Standing height; both are runtime-only and never authored.
Crouching is available to Lua but Door traversal never selects it automatically,
and an admitted low Door crossing selects Crawling itself at 50% of the ordinary
threshold speed before restoring Standing.

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

The additive `promethium.v2` movement contract is
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

## Explicit Furniture use and finishing (#462)

**#523 contract update:** use-enabled definitions require `use_pose` and
`finish_use_pose`. The host, not the callbacks, stages these Poses and releases
occupancy on finish. Furniture `set_pose` calls are errors (including caught or
same-pose calls); general Marker Actions retain it. One production
`World::furnitureUseEligible` query checks capability and target-space fit for both
phases, supplies editor refusal reasons, rejects requests before movement and
rechecks on arrival. Idle arrival is unaffected. Historical Standing-only cleanup
and omitted-cleanup errors below are superseded; full lifecycle/edit protection
is #524. See [current Furniture authoring](lua-furniture-catalogues.md).

The immutable built-in `use-furniture` delegates to the selected owning Lua
Furniture definition's `use(agent, world, marker)`. Paired `use`/`finish_use`
functions derive availability; definitions with neither offer only Idle unless
custom Actions are assigned. Explicit redundant Use furniture assignments are
accepted only at usable definitions and are deduplicated in the selector. No
Action registry is required for the built-in.

All callbacks use the same fresh, budgeted sandbox and atomic validated effects
as custom Actions. Agent views now expose `pose` (`standing`, `sitting`, `lying`,
`crouching`, `crawling`).
Furniture-owned Marker views additionally expose `usable_point` and an immutable
`furniture` view with `id`, `name`, and `definition`. No mutable domain object,
occupancy table or shared VM is exposed. Chair scripts stage Sitting then claim
the selected point; finishing stages Standing and release. Instance occupancy
remains host-managed and per point, independent of immutable definition functions.

Active Furniture use is separate from callback execution, recording the Agent,
Marker, instance, definition and accepted catalogue snapshot. Repeated use at the
same active point does not invoke either callback. Idle or a custom Action at that
point finishes before replacement execution. New journey acceptance and Route
planning retain use; finishing occurs immediately before the first physical
position change. Unreachable journeys, pause and deactivation retain the seat.
Occupied destinations are unavailable to every other Agent for every Action,
including Idle, with an arrival recheck. Intermediate routing remains permitted
(subject to the authored Blocks pathing property). Conflicts produce ordinary
request failure/Route loss, never a script error or partial pose/claim commit.

The host unconditionally restores Standing and releases occupancy after finishing,
even if the callback throws, exceeds instruction/heap budgets, fails validation,
or omits cleanup. Incomplete finishing is rejected before staged device effects
or logs commit. Structured Action-failure diagnostics are published in the same
tick and use the existing interactive-pause/headless-failure policy. Authored
requests/assignments retain built-in identities through YAML/binary documents and
editor request Undo/Redo without serializing callbacks or transient active use.
Structural-edit/reconstruction lifecycle and transactional reload remain separate
slices, as does conversion of legacy demonstration catalogues/implicit-use checks.

Focused public checks: Simulation `markerActions/furnitureUse`,
`markerActions/furnitureUseCompetition`, `markerActions/furnitureFinishFailures`,
`markerActions/furnitureUseDocuments`; Editor `markerActions/furnitureUseWorkflow`.
They cover the real use-to-finish journey, independent sofa points, deterministic
competition, all-Action exclusivity and circulation, idempotence, replacement,
unreachable/physical departure timing, safe views, failure cleanup, authored
request persistence and headless editor selection/history. The fixture is
`src/headless/smoke/fixtures/use.furniture.lua`, independent of demonstration content.

### #462 final Linux verification

Final Release and Debug default builds passed (core, headless, editor and contract
inventory). Unfiltered final CTest ran 110 tests per configuration: **103 passed,
one optional GUI capability skipped, six failed**, exclusively for the seven
unchanged legacy implicit-Furniture checks listed above. All #462 World/document,
Simulation and Editor checks and their CLI/exhaustive contracts passed; no new
failure remains. No tests were bypassed or weakened. `git diff --check` passed.
Windows validation is not claimed. Debug final CTest was run serially after an
earlier cross-configuration contention timeout; final evidence has no timeout.

Final build evidence: Release `0f6e68c251694ef7a75fcb3cd44002bc`, Debug
`e65db6e064f74a4d82f709ad42413af2`. Final unfiltered CTest evidence: Release
`7f541f1cba074c7bbdbe94bb8c1c899e`, Debug
`fc98d13c77f44855a2b66fcaf91dab51`. Full-suite green remains the #467 integration
contract; legacy catalogue/fixture migration and structural/reload lifecycle are
not part of #462.

## Furniture lifecycle and structural-edit safety (#463)

Accepted Furniture movement/deletion finishes affected uses after complete replay
preflight, while the old Marker, owning Furniture and immutable definition remain
accessible. This also handles active uses that never claimed occupancy. Agents
remain at their physical positions for Furniture edits. Rename-only edits and
unrelated uses retain both lifecycle identity and claims through local replay,
without executing callbacks. Lua usable-point claims no longer depend on the
legacy arrival-action enum when being restored. Surrounding Location movement
and Level/Layer target removal finish affected uses after their edit validation.
Existing support/reference protections remain authoritative; refused edits do
not finish use or add document history.

Pending requests to deleted targets retain their stable identity and publish
TargetDeleted cancellation rather than invoking stale Actions. Reset, load and
history reconstruction discard runtime use/claims/Pose without calling either
lifecycle function. Only later simulation of an authored request can execute use.
Local replay retains the accepted catalogue snapshot for surviving active uses;
none of this transient lifecycle data is serialized.

Throwing, budget-exhausted, invalid or incomplete finish callbacks still restore
Standing and release occupancy. Structural-edit failures retain their structured
pending outcome and failure flag across replay, so the next simulation tick
publishes the diagnostic and reports headless failure/pauses rather than silently
clearing the error at tick entry.

Public coverage: new Simulation `markerActions/furnitureStructuralEdits` exercises
accepted/refused movement/deletion, behaviour-reference protection, rename-only
replay, claimed/unclaimed use, unrelated-use preservation, stale Action
cancellation and surrounding edits. Extended `furnitureFinishFailures` covers
move/delete failure cleanup, `furnitureUseDocuments` covers active-use save/load
and Reset without callback replay, and Editor `furnitureUseWorkflow` covers edit
refusal/history, safe movement and move/delete Undo/Redo. Existing
`furnitureUse` covers pause/deactivation, unreachable replanning and exact
physical-departure finishing. All are bounded, headless public-seam checks.

### #463 final Linux verification

Release and Debug default builds passed (core, headless, editor and contracts).
Final unfiltered CTest ran 110 tests per configuration: **103 passed, one optional
GUI capability skipped, six failed**. The failures are exactly the seven unchanged
legacy implicit-Furniture checks already recorded for #462; no #463 check or new
failure remains. Full-suite green and legacy migration remain #467's contract.
Focused lifecycle/document/editor checks passed in both configurations, and
`git diff --check` passed. No Windows validation is claimed.

Final build evidence: Release `77dec36df1024a17859051ad477bf477`, Debug
`1e94bb1c43c0410d9d1f22bcb0ab0880`. Final unfiltered CTest evidence: Release
`0c607048c86e409a8161106f98c8b553`, Debug
`ae3b0c886195442bbed93ab0b1d21015`.

## Transactional Action and Furniture reload (#464)

`World::reloadActionRegistry(path, diagnostic)` and
`World::reloadFurnitureCatalogue(path, diagnostic)` require an already selected
package and a paused World. Both refuse a different basename or UUID. Reload reads
fresh immutable source, validates Lua contracts and stable authored references,
and publishes contextual refusal diagnostics. Furniture additionally preflights
complete construction replay, including usable-point keys, geometry, routes,
Floor support, overlap and assigned Action availability. No live lifecycle
callback runs during preflight. Refusal preserves the loaded package, references,
topology, claims/Pose, modified state and document history.

After successful Furniture preflight, every active use belonging to that catalogue
finishes in stable Agent order with its retained old definition and old Marker
view. Only then may the replacement be installed. Throwing or incomplete old
finishing applies host Standing/claim cleanup, retains the old installed catalogue,
and reports the existing structured Action failure; the next simulation tick
fails/pauses normally. It is not a rollback of successful old teardown callbacks.
Action-registry reload does not touch active Furniture uses.

Callback-only reload keeps the current topology and requests. Changed definition
metadata/layout reconciles through normal validated structural replay, preserving
Furniture/Marker identities and pending selected Actions. Invalidated derived
Use furniture requests produce explicit ActionUnavailable cancellation, not Idle.
An explicit authored Use furniture assignment instead refuses incompatible reload.

The Marker panel exposes **Reload Action registry**, using the selected registry's
source path; the Furniture panel exposes **Reload Furniture catalogue**, using the
saved World's catalogue reference. Both reuse public World workflows and require
pause. Reload changes accepted executable snapshots, not authored package
references, and does not add an undo entry. Selection/assignment edits retain normal
document history. Action history carries only package identity/path, never source
or functions; assignment Undo/Redo uses the currently accepted package. Furniture
history likewise uses the current accepted catalogue for the same UUID. Runtime
uses, Pose/claims and VM state are never persisted or replayed through history.
Resource-backed catalogue preflight also reads fresh files and validates artwork,
rather than silently returning a stale path-only catalogue cache.

Focused public checks are Simulation `markerActions/reload` and Editor
`markerActions/reloadWorkflow`. They cover Action/Furniture success and failure,
identity/reference/placement refusal, old-function teardown ordering, valid layout
reconciliation, stable pending requests, explicit cancellation, cleanup failure,
running refusal, unchanged history on reload and no old executable replay on
Undo/Redo. All checks are headless and bounded.

### #464 final Linux verification

Release and Debug default builds passed (core, headless, editor and contracts).
Final unfiltered CTest ran 110 tests per configuration: **103 passed, one optional
GUI capability skipped, six failed**. These are the unchanged legacy implicit-use
Simulation/Routing/Render failures documented for #463 (seven scenario checks,
six CTest entries); full-suite green and legacy migration remain #467's contract.
All #464 checks and Editor functional/CLI/exhaustive contracts passed in both
configurations. `git diff --check` passed. No Windows validation is claimed.

Final build evidence: Release `fde5a62c6cf84b64838f4cee403634d4`, Debug
`3389068a11f34a7b82ca9d477b512b38`. Final unfiltered CTest evidence: Release
`347f20ccd3f245d9b0dbd1c3ef714548`, Debug
`61f4b84b56014925b5623c5455e966a6`.

## Live editor Marker requests (#469)

Before this fix, `applyAgentPathEdit` routed every Marker destination with
`startPathing` through the paused-only authored request seam, so clicking a
destination Marker (or using **Path to selected Marker**) while the simulation
ran was silently refused with only a log warning and left the editor in
destination-selection mode. Authored request state is reset-persistent and is a
document edit; a live editor request is not.

The shared editor seam `applyAgentPathEdit` now selects the seam from the
World's state: a paused World still authors a reset-persistent document request
through `commitAgentMarkerActionRequest`, while a running World issues a
transient runtime request through `World::moveAgentToMarker` with the selected
Action, exactly as a behaviour-issued request does. The runtime path changes no
authored state and adds no history. A refusal produces a status-specific
diagnostic, which the editor surfaces visibly; a destination-selection attempt
then ends whether it succeeded or was refused. `commitAgentMarkerActionRequest`
itself still refuses while running, so only the authored request requires pause.

The seam lives in `pf-agent-editing` (`requestAgentMarkerAction`,
`applyAgentPathEdit`) so it is exercisable headlessly; the GUI keeps a thin
wrapper that reports the returned diagnostic. Documented workflow remains:
runtime requests are transient and authored requests persist their Action.

Focused public check: Editor `markerActions/liveDestinationEdit`. It covers a
live destination edit that starts and completes movement with no history or
dirty state, a refused live edit that returns a diagnostic and touches no
history, the authored seam's running refusal, and a paused edit that authors one
undo entry and dirties the document.

Verification: Release default build and unfiltered CTest reported **110 passed,
0 failed**; the new check also passed in the Debug build of the affected
targets. `git diff --check` is clean.

## finish_use device requests at departure (#470)

A `finish_use` callback may legitimately request a device operation, for example
turning a reading light off when the Agent stands up. Departure finishing runs
inside `Agent::setPosition` before the physical position commits, so the Agent is
still at the usable point, but `startPathingInternal` had already set
`MovingToVertex`. The staged `world.request_device` was therefore refused by the
Idle/WaitingForTraversal eligibility gate, the atomic rejection discarded the
staged Standing/release, and `finishFurnitureUse` escalated that ordinary
refusal into `ScriptExecutionFailure::ConversionError`, pausing the simulation or
failing headless advancement.

`World::interactionRequestEligible` now treats the Agent whose `finish_use` is
executing as still eligible for a device request, so both staging and the
committed `requestInteraction` are judged from the Agent's still-physical
position. The exception is scoped to the finishing call
(`World::mFinishingFurnitureUseAgent`) and relaxes no other authority: sector,
reach, activation, Mobility/Buttons, Access and typed-binding permissions still
apply.

`finishFurnitureUse` no longer rewrites an ordinary `ActionFailed` with
`scriptFailure == None` into a conversion error. That escalation only ever fired
for ordinary refusals, because callback failures, budget exhaustion and the
incomplete-Standing/occupancy cleanup check already set their own classification
and failure flag inside `applyActionResult`. If the staged pose/occupancy cleanup
is well-formed but the device request is legitimately refused, the refusal is
published as an ordinary `ActionFailed` with a diagnostic, the host still
restores Standing and releases occupancy, and simulation continues.

Focused public check: Simulation `markerActions/furnitureFinishDevice`. It covers
a reachable device request admitted at physical departure that completes
asynchronously, and an out-of-reach device request that stays an ordinary
non-script refusal without pausing.

Verification: the full Simulation smoke module reported **156 passed, 0 failed**
in the Release tree, and the new check also passed in Debug. `git diff --check`
is clean.

## Action registry beside the saved World (#471)

A World document stores only an Action registry's basename and expected UUID and
resolves it beside itself on reopen. The Marker **Agent Actions** panel passed its
raw text-field path straight to `World::selectActionRegistry`, which accepted any
readable `.actions.lua` from any directory and recorded only `path.filename()`. A
World saved with such a reference then failed to reopen with `Missing Action
registry dependency: <basename>` until the file was copied beside it.

Selection now mirrors the Furniture catalogue workflow. The World records the
directory of its last successful save or load (transient and never serialized).
`World::selectActionRegistry` rejects a registry whose canonical parent is not that
directory with a clear diagnostic, and treats a relative reference as sitting
beside the World; an unsaved World has no document location to violate and is
unchanged. The panel seam `commitActionRegistrySelection` applies the same
adjacency rule before capturing history, so a user-entered absolute or relative
path from another directory is refused without attaching anything or adding an
undo entry. `ActionRegistry::filenameIsValid` centralizes the `.actions.lua`
basename contract used by selection, load and deserialization.

Focused public check: Editor `markerActions/registryDirectory`. It covers the
unsaved-World refusal, the World and panel refusals of a parseable registry in
another directory (with unchanged history), acceptance of a bare relative
reference beside the saved World, and a save/reopen round trip that resolves the
adjacent registry and its assignments.

Verification: the full Editor smoke module reported **208 passed, 0 failed** and
the full Simulation smoke module reported **156 passed, 0 failed** in the Release
tree; Persistence reported **95 passed, 0 failed** and World **48 passed, 0
failed**. `git diff --check` is clean.
