# Broken coupled Shuttles (#320)

Open `resources/test-worlds/broken-shuttles.world.yaml`. It has two coupled
Carriages, each with its own capacity and two Doors, and disconnected platform
approaches. The whole Shuttle starts Broken; there is no independent Carriage or
owned-Door failure toggle.

## Demonstration

1. Run the simulation. The remote observer initially plans to Destination A
   without knowing the remote failure. On entering Origin A it discovers the
   Broken service, enters mandatory Route planning, and loses its Route.
2. Select the Shuttle on Layer 2. Its Selection panel shows **Broken**, the
   coupled vehicle's physical x position, **Initially Broken** (paused authoring),
   and **Live Broken** (runtime only). An amber warning sits above the physical
   vehicle, including in wireframe; it does not replace its geometry.
3. Clear **Live Broken**. Nearby Agents learn restoration on their next local
   observation. Give the two Agents Paths to Destination A and Destination B
   respectively (select each Agent and use the existing path-selection controls).
4. With both passengers aboard, set **Live Broken** between Stops. Both Carriages
   freeze together. Passengers keep their positions, capacity ownership and
   accepted journeys. Calls and destination-selection operations fail.
5. Restore live service: travel resumes at the frozen position and each passenger
   exits through its Carriage's selected Door. Repeat at a Stop with open Doors:
   alighting remains possible but new boarding is refused. Breaking during Door
   opening freezes the partial aperture; it cannot open further until restored.
6. Reset: authored Broken returns and every Agent's condition memory is cleared.
   Live changes neither dirty nor overwrite the authored World. Paused changes to
   **Initially Broken** are saved and undoable.

## Observation and safety contract

An Agent observes from the Shuttle Transit or any of its landing Locations,
including a disconnected/non-representative Carriage approach and a passing Agent
with no Shuttle Path. It remembers vehicle Broken state, x position, alignment,
and locally observed landing apertures. Routing uses local observations or last
memories, not remote live failure/restoration. Fresh whole-vehicle observations
supersede stale failure knowledge from other approaches without updating their
remote Door apertures. Memories persist until re-observation or reset.

Unusable current Paths require Route planning and Route loss if no replacement
exists. Restored service or another usable change uses voluntary planning and
ordinary Route persistence. Onboard passengers retain accepted journeys rather
than replanning into a capacity-losing escape. A Carriage cannot use another
Carriage's open Door; closed/partial Doors never admit alighting while Broken.
Existing admitted Door crossings still complete under their permits.

World schema 39 adds optional `initiallyBroken` on Shuttle records. Older Worlds
without it default to working. YAML, binary reset snapshots, replay and document
history persist only authored condition; runtime memories are never serialized.

## Headless verification

All checks are directly selectable with `--check <name>` (one per invocation):

- `pf-smoke-simulation`: `shuttles/brokenFreezeAndRecoverPassengers`,
  `shuttles/brokenStopDoorsAndSelectorSafety`, `shuttles/brokenLocalMemory`,
  `shuttles/brokenRoutePlanning`.
- `pf-smoke-persistence`: `shuttleBrokenLifecycle` (includes loading this demo).
- `pf-smoke-editor`: `shuttles/brokenControlsAndHistory` (CPU-only ImGui controls,
  dirty state, undo/redo and disabled-scope balance).
- `pf-smoke-render`: `shuttles/brokenWarningPreservesPosition` (CPU-only drawing,
  frozen geometry, warning clearance, wireframe and restoration).

No check opens a window or file dialog.
