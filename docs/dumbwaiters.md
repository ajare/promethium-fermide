# Dumbwaiters (#375–#379)

A Dumbwaiter is a non-passenger, fixed 1-cell-wide, 2-Level-high Transit. Paint
**Dumbwaiter** on a shaft Layer behind two supported landing cells at the same x
position. Rooms, Corridors and Facades on the immediately front Layer can supply
one shared multi-Level Location or two separate Locations. Both cells need Ground
or Walkway support; Force Bridges, occupied shafts and conflicting apertures refuse
placement without changing the World. Landing controls use the independent
right-first placement policy below; a valid left fallback needs no neighbouring
button cell.

Selection on the shaft Layer exposes **Initial Stop** (Lower by default),
**Travel time (seconds)** (2 by default, inclusive 0.1–60), and **Delete Dumbwaiter**.
Creation, configuration and whole-unit deletion participate in ordinary document
undo/redo. The car starts at the authored Stop with that shutter fully open and
the other closed. Runtime Selection exposes **Press lower landing** and **Press
upper landing**, current phase, car Level and both shutter states/progress. The
landing buttons show green here, amber elsewhere, or red busy at both landings.
Car geometry is procedural and reuses the existing shaft surface and BoothWindow
resources. There is no passenger path, journey queue or traversal resource.

`World::pressDumbwaiterLanding(id, stop)` (Stop 0/1) submits a typed
`PressDumbwaiterLanding` Device command; the equivalent `submitDeviceCommand`
accepts `dumbwaiter` identity and `stopIndex`. Invalid targets return no handle.
Use `lookupDeviceOperation` for Running/Succeeded/Rejected/Cancelled outcomes and
`lookupDumbwaiter` for phase, car position, shutters and button state. An idle press
at either landing selects the Stop opposite the car: calling an absent car or
sending a present one has the same destination and automatic arrival opening.

Admission reserves busy immediately, before any physical tick. Subsequent presses
are separate Rejected operations, never coalesced, queued, reversed or replayed.
Call order determines simultaneous user-command admission. The Simulation
coordinator fully closes departure (48 fixed ticks / 0.8 seconds), moves at constant
speed for the authored duration (default 120 ticks / 2 seconds), then opens arrival
(48 ticks). Opening starts only after arrival; success means fully open. Both
shutters stay closed during travel, and the idle arrival stays open indefinitely.
Pause freezes accepted work; resume and time scaling use ordinary fixed ticks.
Terminal operations retire at tick boundaries under ADR 0013; cancellation and
completion remain observable through `consumeSimulationEvents`.

Initial Stop/travel-time edits reset only this device and cancel its operation.
Whole-unit deletion preserves unrelated Agents/devices, removes owned shutters,
and cancels accepted work. Successful structural topology reconciliation resets
surviving Dumbwaiters; pause alone and refused edits do not. Reset/load restore
authored initial state. Runtime controls do not change documents or history.

Owned apertures have no back-side panel and cannot be independently edited,
resized, moved, copied, deleted or toggled. Independent owned-aperture movement
remains refused; use whole-unit movement instead. Surrounding structural edits
reconcile the complete dependent unit as described below; unrelated objects retain
normal editing.

World schema **47** adds one `dumbwaiter` construction record carrying stable
World-owned `id`, shaft `layer`, lower `y`, `x`, `initialStop` (0/1) and
`travelSeconds`, plus the World identity high-water mark `nextDumbwaiterId`.
Children and shutter state are derived, not independently serialized. YAML and
binary use the same ordinary document validation and replay; schema-46 and older
Worlds remain supported. Saving before the first tick or during closing, travel or
opening writes exactly the authored document, never runtime position, progress,
requests or operations. No schema change is needed for the runtime-only cycle.
Loading and Reset restore the authored presentation.

## Independent physical landing placement (#434)

Each one-cell landing independently prefers host `X+1` at offset `0.0`, otherwise
`X` at offset `0.0`. The host must belong to that landing Location and have Ground
or Walkway support. Retained shared walls and controlled Bulkhead thresholds
invalidate straddling candidates, regardless of runtime opening; removed shared
walls and outer boundaries do not waive host/support checks.

Both controls join the canonical allocator and may share same-Location stacks of
up to four independently operated Buttons. The owning key is the complete authored
shaft geometry and Dumbwaiter type, with lower/upper roles ordered by Level. Each
control retains its own command, identity and permission requirement. Graph
co-location shares a normal-height approach, not authorization or operations.
Production rendering and mouse targeting use the physical Button shape; the old
aperture-inset rectangle is no longer drawn. Standalone BoothWindow back-side
panels remain invisible and are not physical allocation demands.

Creation, movement, clipboard, deletion, dependent structural reconciliation,
load, replay and undo/redo reconstruct through the same policy. Removing aperture
support removes the complete unit and both demands; removing only a potential
host must preserve a valid allocation or refuse the edit. Invalid edits and
incompatible loads are transactional refusals, never missing required controls.
Legacy landing requirements and surviving Marker identities are retained when
physical children shift older object slots. No placement/stack state is serialized,
and car motion never changes landing placement.

## Agent landing operation (#377)

Each landing owns one physical Button and a distinct Interaction point
(`getLandingButton(stop)`), with inclusive 0.25 world-unit reach and the existing
one-tick press duration. Its Interaction point remains at the allocated centre X
and walkable Level, even when its visible Button is stacked. There is no
shaft-side control. `World::requestDumbwaiterLanding(id, stop, actor)`
uses the typed `PressDumbwaiterLanding` binding and ordinary Interaction outcomes.
Agent Selection exposes a manual landing press, disabled when ineligible or busy.
Requests never auto-approach, move the Agent, create delivery behaviour or route
passengers.

Admission and activation require an active Agent in the landing Location, within
reach, whose current effective Buttons Mobility use is not Cannot use. Individual
Mobility properties override tags. Each landing independently requires every
current direct/Permission-set grant in its requirement (ADR 0015); empty is the
default. Edit these through the landing requirements in Dumbwaiter Selection or
`setInteractionPointPermissionRequirement`. Paused eligibility changes reject or
cancel unactivated presses without changing car/shutter targets. Accepted cycles
survive Agent departure/deactivation, grant loss and permission tightening;
permission-only edits preserve physical progress and the accepted operation.
User runtime controls remain independent of Agent eligibility but share the same
busy/interlock gate. Simultaneous activations follow deterministic Interaction
point order, and all competing pending presses are refused, never deferred into
a later journey.

Schema **48** adds `lowerLandingPermissionRequirement` and
`upperLandingPermissionRequirement` arrays of Access permission IDs to the authored
construction record. Schema-47 Dumbwaiters default to empty requirements. YAML,
binary, construction replay, Reset and ordinary document undo/redo preserve both
requirements; deleting a permission removes it from both. Unknown, duplicate,
zero or malformed references are rejected transactionally. Whole-unit clipboard remapping follows the identity/name conventions below. Structural/configuration cancellation
also retires pending interactions; removed devices retain no live request.

## Whole-unit movement and clipboard (#378)

Paused Dumbwaiter Selection provides destination shaft Layer, x, lower Level,
and **Move Dumbwaiter**. `World::planMoveDumbwaiter` and
`applyDumbwaiterMove` use the same complete destination validation as creation,
ignoring only the moving unit's own footprint. Application rechecks stale plans.
Invalid destinations and same-position moves leave accepted cycles and pending
presses unchanged. Successful moves cancel that unit's work, rebuild both owned
apertures/buttons at the new landings, and restore its authored initial Stop.
Identity, timing and both landing requirements survive; unrelated cycles are
not reset. The old child/control handles are invalid, so callers refresh through
`lookupDumbwaiter` after a successful edit.

Select the shaft in Sector mode and use ordinary **Copy/Cut/Paste** (including
Ctrl+C/X/V). Paste uses the selected visible shaft Layer and cursor's lower cell.
The clipboard contains fixed dimensions, initial Stop, travel time and both
landing requirements, never source device identity or runtime work. Pasting a
mid-cycle source creates a new idle unit without disturbing the source. Both
requirements resolve before any placement: the same World resolves stable IDs
(across renames); another World resolves exact permission names, never coincident
local IDs. Missing, duplicate or malformed references refuse the entire paste,
without creating permissions or grants. `CreateDumbwaiterOptions` also accepts
both landing requirements for atomic public World creation/preflight.

Schema **49** adds chronological `moveDumbwaiter` construction records (identity,
shaft Layer, lower y and x). They preserve producer/object-slot ordering even
when destination Locations were authored later than the unit, or another unit
is subsequently placed at the vacated site. Configuration and requirements stay
on the unit's authored producer; children are derived afresh during replay.
Undo/redo, Reset, YAML and binary load restore coherent authored placement and
initial presentation without stale runtime handles. Schema 48 and older remain
loadable. Whole-unit deletion removes movement records and preserves surviving
object slots.

## Surrounding structural edits (#379)

Public Location and Walkway edit plans expose dependent whole-unit deletion in
normal editor consequences. Removing either landing Location, cropping its cell,
or removing its permanent Walkway support removes the shaft, car, both apertures,
both buttons and outstanding work together. A resize that retains both supported
landing cells retains the unit. Existing safety refusals (occupied Walkways,
active connected resources, invalid footprints) still apply before commitment.

Level deletion removes intersecting Sectors and units dependent on removed landing
Locations; surviving units above the deletion compact down with both adjacent
Stops. Layer deletion removes units on that Layer and on the Layer immediately
behind it, and compacts surviving units with their landing Layer. Canonical replay
keeps whole-unit moves in order with Transit producers. Surrounding reconciliation
rebuilds previously moved survivors at their final placement, retaining historical
aperture slots as tombstones where their Location survives. Removing obsolete
landing support therefore cannot strand a currently supported unit or replay an
old shaft over a reused site.

Successful reconciliation cancels accepted operations and pending Agent presses,
with cancellation events observable through `consumeSimulationEvents`. Surviving
units restore authored timing, initial Stop and derived shutters; removed/restored
shutter and landing-control handles are invalid and callers must refresh them.
Rejected edits leave in-flight work untouched. Pause and permission-only changes
remain non-destructive. Unrelated Agent runtime properties, grants and replay state
follow the existing structural-edit preservation policy, not Simulation Reset.

Ordinary document undo/redo restores complete authored units, including both
independent landing requirements. YAML/binary save/reopen and Reset replay the
edited authored result, never orphan children or in-flight work. No schema bump
is required: child components remain derived from the unit record.

Headless coverage is in World, Persistence, Editor and Render's BoothWindows
translation units and Simulation's Dumbwaiters translation unit, using real public
World ticks/operation queries, document history/Selection, and production draw
commands. It covers both call/send directions and initial Stops, timing/intermediate
positions, every busy phase and pre-tick contention, idle-open, pause/time scaling,
lifecycle cancellation and unrelated-state preservation, mid-cycle YAML/binary
loads, runtime Selection and moving-car shutter/depth clipping. Standalone
BoothWindow and passenger Lift coverage remains in the existing suites. Use the
[Linux validation procedure](linux-smoke-validation.md) for final Release/Debug
coverage; tests are CPU-only and non-interactive.
