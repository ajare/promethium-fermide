# Airlock chambers and single-Agent journeys (#322, #323)

The Airlock palette tool paints a one-Level, positive whole-cell chamber on the
visible Layer. It requires empty chamber cells and a walkable Room or Corridor
end immediately on each side. Rooms, Corridors, and mixed pairs are supported;
Facades, Backgrounds, other Transits, overlap, and carving are refused before
World mutation. Unlike other Transits, an Airlock's Stops are on its own Layer.

Select the chamber to inspect its width and derived capacity (one Agent per
cell) and configure its cycle duration while paused. The duration defaults to
3 simulated seconds and accepts finite values in inclusive range [1, 10].
Creation and timing edits participate in existing document undo/redo.

World owns the chamber, its two generated Bulkhead Doors, and three fixed
physical buttons (one at each outside entrance and one inside). The adjoining
Location wall ends are opened automatically. Their previous authored states,
including an originally open end, are retained separately for later restoration.
The owned Doors cannot be independently edited, opened, removed, or authored
Broken; the buttons cannot be independently moved or removed. The internal
button cannot acquire a Permission requirement.

A lone Agent can route through the chamber in either direction. Both threshold
edges reference one Airlock Traversal resource, which owns queue tickets,
admission reservations, crossing authority, and occupants. Fixed buttons target
the Airlock, never individual Doors. Outside buttons request entry from their
own side; the internal button requests exit opposite entry. Authored Agent
placement inside the chamber remains refused: entry must establish occupancy
through coordinated traversal. Ordinary Bulkheads and other Transit landing
conventions are unchanged.

At least one Door stays fully closed on every tick. Admission waits for a fully
open entrance, and its crossing authority protects closure until boarding
completes. Boarding closes the entrance; the authored cycle starts precisely
when both Doors become fully closed. Neither Door opens before that cycle
finishes. The internal interaction serves the committed opposite exit. Capacity
is retained throughout exit crossing and released only on completed exit; the
exit closes and another cycle runs before subsequent entry, including from the
previous exit side. Empty calls close after the normal Bulkhead timeout and
cycle without an internal press. Initial creation/load/reset starts ready for
immediate first entry. Pause freezes Door motion and remaining simulated seconds
and retains occupied capacity for resumed routing.

Basic directed estimates use authored chamber walking (including the internal
button approach), two required interactions, Door motion, and cycle delay.
Remote live Door, queue, and cycle state are not consulted. Preference and
complete authorization policy belong to the dedicated follow-up slice.

World schema 40 persists the chamber's geometry, timing, identity through the
construction stream, and prior wall states. YAML and binary load, reset, and
construction replay regenerate the owned Doors and controls deterministically.
They start empty, with both Doors closed and the initial cycle complete. Public
`SimulationSnapshot::airlocks` reports the chamber identity, width, capacity,
timing, both door states, control identities, entry side, reservations, crossing
requests, occupants, and availability. Existing Agent/request/permit snapshots
expose traversal progress. Rendering shows occupants and remaining cycle seconds.

Batch admission/fairness, complete authorization/preferences, structural chamber
editing/restoration workflows, pressure/scanning simulation, and Broken support
remain follow-up work, not part of #323. Device commands and traversal
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
