# Authored Airlock chambers (#322)

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

**This intermediate slice does not provide journeys.** Chambers are excluded
from traversal topology, have no ordinary Bulkhead traversal resources, refuse
Agent placement, and reject control operation. The generated buttons target the
Airlock, not individual Doors. It is impossible to use an ordinary fully-open
Bulkhead bypass to enter the chamber. Ordinary Bulkheads and other Transit
landing conventions are unchanged.

World schema 40 persists the chamber's geometry, timing, identity through the
construction stream, and prior wall states. YAML and binary load, reset, and
construction replay regenerate the owned Doors and controls deterministically.
They start empty, with both Doors closed and the initial cycle complete. Public
`SimulationSnapshot::airlocks` reports the chamber identity, width, capacity,
timing, both door states, control identities, occupants, and availability.

Movement, resizing, deletion/restoration workflows, journeys, countdown
advancement, pressure/scanning simulation, and Broken support are not part of
#322. Device commands and future traversal coordination remain separate (ADR
0001); this slice does not create an independent edge-owned coordinator.

Headless coverage is owned by the existing World, Persistence, Render, and
Editor smoke modules, registered under `airlocks/*`. It uses public World and
snapshot APIs, document-history commands, YAML/binary replay, and renderer
command output, with no display server or blocking dialogs.
