# Airlock chambers and journeys (#322–#327)

The Airlock palette tool paints a one-Level, positive whole-cell chamber on the
visible Layer. It requires empty chamber cells and a walkable Room or Corridor
end immediately on each side. Rooms, Corridors, and mixed pairs are supported;
Facades, Backgrounds, other Transits, overlap, and carving are refused before
World mutation. Unlike other Transits, an Airlock's Stops are on its own Layer.

Select the chamber to inspect its width and derived capacity (one Agent per
cell) and configure its cycle duration while paused. The duration defaults to
3 simulated seconds and accepts finite values in inclusive range [1, 10].
Creation and timing edits participate in existing document undo/redo.

World owns the chamber, its two generated Bulkhead Doors, and two fixed
physical buttons, each centred in the cell immediately outside its entrance,
matching standalone Bulkhead Door buttons. There is no internal button. The adjoining
Location wall ends are opened automatically. Their previous authored states,
including an originally open end, are retained separately for later restoration.
The owned Doors cannot be independently edited, opened, removed, or authored
Broken; the outside buttons cannot be independently moved or removed.

A lone Agent can route through the chamber in either direction. Both threshold
edges reference one Airlock Traversal resource, which owns queue tickets,
admission reservations, crossing authority, and occupants. Fixed buttons target
the Airlock, never individual Doors. Outside buttons request entry from their
own side; the exit opposite entry opens automatically after the closed-door cycle. Authored Agent
placement inside the chamber remains refused: entry must establish occupancy
through coordinated traversal. Ordinary Bulkheads and other Transit landing
conventions are unchanged.

At least one Door stays fully closed on every tick. Admission waits for a fully
open entrance, and its crossing authority protects closure until boarding
completes. Once the boarding window has ended and selected Agents have boarded,
the entrance closes; the authored cycle starts precisely
when both Doors become fully closed. Neither Door opens before that cycle
finishes. For an occupied chamber, the committed opposite exit starts opening
on the tick the cycle completes, without any Agent interaction. Capacity
is retained throughout exit crossing and released only on completed exit; the
exit closes and another cycle runs before subsequent entry, including from the
previous exit side. Empty calls close after the normal Bulkhead timeout and
cycle back to readiness without opening an unrequested opposite Door. Initial creation/load/reset starts ready for
immediate first entry. Pause freezes Door motion and remaining simulated seconds
and retains occupied capacity for resumed routing.

Outside buttons expose independent Access permission requirements in their
Selection panels. Both requirements use all-members authorization and persist
through document history, YAML/binary load, construction replay, and reset.
Buttons Mobility use is checked before admission for the outside controls:
Cannot use refuses routing and stale runtime Paths; Only if no other option uses
the ordinary two-pass route selection and permits its selected journey.

Closed or unobserved entrances require authorized outside operation. An adhering
Agent also declines a protected locally open entrance; a non-adhering Agent may
consider it usable without acquiring operation permission. Neither willingness
nor grants bypass coordinated admission, direction, capacity, or interlocks. The opposite
Room or Corridor's Location requirement is checked before admission independently
of control requirements and adherence. Once admitted, later grant loss or tighter
control/Location requirements cannot revoke the committed opposite exit.

Directed objective estimates include chamber walking, outside button approaches
and required entry interaction duration, Door closure/opening, and one cycle.
Automatic exit has no internal-button interaction or walking detour.
Perceived cost weights waiting and interaction through existing Agent preferences;
objective timing is unchanged. Locally open entry omits unnecessary outside
operation. Queue/crowding observations are limited to the current approach;
opposing queues and unobserved remote live Door, reservation, and cycle state
never determine the estimate. Remote entrances use authored baseline timing.

World schema 42 persists independent outside requirements alongside the chamber's
geometry, timing, identity through the construction stream, and prior wall states.
Version-40 chambers load with unrestricted outside controls; requirement-bearing
older-schema documents and dangling/duplicate requirements are refused atomically. YAML and binary load, reset, and
construction replay regenerate the owned Doors and two outside controls deterministically.
The former internal InteractionPoint ID is kept unused during construction so
permission references on subsequent controls in older documents remain valid;
no internal button, interaction point, or topology vertex is regenerated.
They start empty, with both Doors closed and the initial cycle complete. Public
`SimulationSnapshot::airlocks` reports the chamber identity, width, capacity,
timing, both door states, control identities, entry side, reservations, crossing
requests, occupants, and availability. Existing Agent/request/permit snapshots
expose traversal progress. Rendering shows occupants and remaining cycle seconds.

Capacity-limited batches and opposing ticket-ordered queues are supported.
The oldest waiting ticket chooses the entry side. Same-direction arrivals may
reserve previously unused slots during a bounded boarding window, including
while the entrance opens. Membership freezes when capacity has been selected or
when the opening time plus normal Bulkhead open dwell expires (currently 6 + 5
simulated seconds from opening start). Arrivals never extend this deadline;
selected Agents may finish boarding after it. An underfilled batch does not wait
indefinitely for more Agents. Opposing queues wait for the committed batch to exit.

Cancelled, inactive, or expired pre-entry members lose their reservations
without replacement within that batch, even while other never-selected slots
remain available. If nobody enters, the entrance honours
its normal Bulkhead timeout, closes, and cycles back to readiness (#325).

Occupants retain capacity, physical position when inactive, and the exit opposite
entry. Destination changes through World movement commands are deferred until
completed disembarkation; replacement routing starts from that exit. Pause retains
selected admissions and occupied journeys rather than selecting a new route inside.
Cycling and automatic exit opening continue even when every occupant is inactive;
inactive occupants remain in place and retain capacity. Active occupants may leave
independently, but no new batch
enters until every occupant has left. Reactivation resumes the retained journey;
there is no automatic activation or passenger removal. Reset clears reservations,
operators, occupants and cycling while preserving authored geometry and timing.

## Structural editing (#327)

Move chambers by dragging their interior or editing x/Level in Selection. Resize
from either horizontal edge or the Selection width field; height remains one Level.
Delete through Selection or the existing Delete shortcut. Commands participate in
normal document history and refresh selection, topology, capacity, and generated
Doors/buttons together. Per-chamber timing and independent outside requirements
survive edits.

Public `planResizeAirlock`, `planRemoveAirlock`, and `applyAirlockEdit` use the
existing candidate-World/construction-replay boundary. Geometry has the same
preflight as creation, with the old chamber removed from the candidate first.
Plans are revalidated on application; invalid edits do not alter authored or live
state. Application requires pause. Occupants and either threshold crossing remain
protected even when paused; structural aggregate replay also refuses while another
Airlock has occupants/crossings rather than destroying its committed journey.
Empty waiting queues and pre-entry reservations do not prohibit edits: replay
cancels old handles and carries destination intent for normal timed Route planning
on resume, with Route loss if changed topology makes the destination unreachable.
There is no forced removal, evacuation, or independent Door editing.

Obsolete adjoining ends regain their saved authored states. Schema 42 adds the
`airlockWallRestoration` flag on a `removeWall` construction record to retain an
originally open end even after its adjoining chamber is moved/deleted. These
single-end restoration records replay before dependent Transits, validate their
Location/Level/side, and are rejected in older-schema documents. Ordinary shared
wall editing remains unchanged. YAML/binary reload, Reset, and document-history
replay preserve restoration information and configuration.

Pressure/scanning simulation and Broken support remain follow-up work. Device commands and traversal
coordination remain separate (ADR 0001); edges never own a coordinator.

Headless coverage is owned by the existing Simulation, World, Persistence,
Render, and Editor smoke modules, registered under `airlocks/*`. It uses public World and
snapshot APIs, document-history commands, YAML/binary replay, and renderer
command output, with no display server or blocking dialogs.

## #323 validation

Final Linux/GCC GUI-enabled default builds passed in Release (`build-linux`)
and Debug with `PF_HIGH_ANALYSIS=ON` (`build-linux-validation/debug`). Complete
CTest inventories passed in both configurations at `--parallel 4`: 85 registered
tests, no failures, and only the optional vendored GUI capability smoke skipped.
`DISPLAY` and `WAYLAND_DISPLAY` were unset. Focused development coverage included
both directions at widths 1, 2, and 5 and cycle durations 1, 3, and 10; every-tick
interlocks/crossing safety, internal interaction ordering, empty timeouts and
same-side reopening, exit-side readmission, pause/resume, live reset, authored
journey reload, directed baseline estimates, and countdown/occupant rendering.
Ordinary Bulkhead and the affected modules' CLI/ownership contracts passed.
`git diff --check` passed. Windows validation is not claimed.

## #326 validation

Full incremental GUI-enabled Linux builds passed in Release (`build-linux`) and
Debug with `PF_HIGH_ANALYSIS=ON` (`build-linux-validation/debug`). Both complete
CTest inventories passed at `--parallel 4`: 85 registered tests, zero failures,
and only the optional vendored GUI capability check skipped. Displays were unset;
all added checks are headless and bounded. Focused checks cover independent outside
requirements, adherence, forbidden and last-resort Buttons, Room/Corridor entry
requirements, stale Paths, grant loss and tightening after admission, alternative
route selection, local/opposing queue isolation, YAML/binary/reset/replay, and
editor undo/redo. Module CLI contracts and ownership/compile audits also passed.
`git diff --check` passed. Windows validation is not claimed.

## #327 validation

Incremental affected-module builds and focused World, Simulation, Editor, Render,
and Persistence checks passed, followed by all ten affected module/CLI contracts.
Full GUI-enabled Linux/GCC default builds and all 85 CTests passed in Release
(`build-linux`) and Debug with `PF_HIGH_ANALYSIS=ON`
(`build-linux-validation/debug`), at `--parallel 4`; the optional vendored GUI
capability smoke was the only skip. After extending waiting-edit coverage to
resize/delete, both final default builds and the affected Simulation registry,
CLI contract, and compatibility contract were rerun successfully. Displays were
unset throughout. New coverage includes invalid-edit atomicity, open/closed
walls, move/resize/delete, entry and exit crossings, paused occupancy protection,
waiting reservation cancellation, route recovery/Route loss, history undo/redo,
YAML/binary/reset/replay, saved restoration flags, and renderer output.
`git diff --check` passed. Windows validation is not claimed.
