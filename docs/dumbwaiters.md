# Dumbwaiters (#375–#377)

A Dumbwaiter is a non-passenger, fixed 1-cell-wide, 2-Level-high Transit. Paint
**Dumbwaiter** on a shaft Layer behind two supported landing cells at the same x
position. Rooms, Corridors and Facades on the immediately front Layer can supply
one shared multi-Level Location or two separate Locations. Both cells need Ground
or Walkway support; Force Bridges, occupied shafts and conflicting apertures refuse
placement without changing the World. No neighbouring button cell is required.

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
resized, moved, copied, deleted or toggled. Delete the complete unit first when
changing its landing Location footprints or removing Levels/Layers. Independent
owned-aperture movement is refused pending full unit movement support; unrelated
objects retain normal editing. Removing required Walkway support is transactionally
refused. Full unit movement,
clipboard and dependent deletion/reconciliation of surrounding footprints remain
separate follow-up tickets.

World schema **47** adds one `dumbwaiter` construction record carrying stable
World-owned `id`, shaft `layer`, lower `y`, `x`, `initialStop` (0/1) and
`travelSeconds`, plus the World identity high-water mark `nextDumbwaiterId`.
Children and shutter state are derived, not independently serialized. YAML and
binary use the same ordinary document validation and replay; schema-46 and older
Worlds remain supported. Saving before the first tick or during closing, travel or
opening writes exactly the authored document, never runtime position, progress,
requests or operations. No schema change is needed for the runtime-only cycle.
Loading and Reset restore the authored presentation.

## Agent landing operation (#377)

Each landing owns one centred Interaction point (`getLandingButton(stop)`) at
`(x + 0.5, Level)`, with inclusive 0.25 world-unit reach and the existing one-tick
press duration. The visible button remains immediately right of its aperture;
there is no shaft-side control. `World::requestDumbwaiterLanding(id, stop, actor)`
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
zero or malformed references are rejected transactionally. Clipboard remapping is
out of scope until the unit move/copy ticket. Structural/configuration cancellation
also retires pending interactions; removed devices retain no live request.

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
