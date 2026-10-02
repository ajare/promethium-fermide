# Broken extensible Ladders and Force Bridges (#316)

Only extensible Ladders (Room objects and Transit sectors) and extensible Force
Bridges expose the condition controls. Ordinary Ladders and non-extensible
bridges remain unchanged.

## Demonstration

Open `resources/test-worlds/broken-extensibles.world.yaml` in the editor.

- The extensible Ladder at x=1 starts Broken and retracted. The lower observer
  discovers it locally, plans a detour over the ordinary Ladder at x=8, and
  arrives at the upper-left Marker.
- The Force Bridge across x=2..3 starts Broken and fully extended. The upper
  observer crosses it normally. Both devices show an amber warning above their
  full footprint while their physical geometry remains visible.
- Select either extensible device and use **Live Broken** to restore/break it.
  The physical extension percentage is shown separately from the condition.
  Restore the Ladder to permit a fresh extension request; use an initial-state
  edit and reset to repeat the authored scenario.
- To demonstrate partial breakage, pause, author the Ladder or Bridge as working
  and initially retracted, reset, then run an Agent toward the upper-left
  destination. Break during extension: motion freezes, the pending command
  fails, and the waiting Agent enters Route planning. Restore to resume motion
  without retracting or resetting position. A fresh destination request can use
  the device again.

## Contract

Broken freezes physical extension and rejects operation commands. Exactly fully
extended devices remain usable, including a retract direction selected before
any physical movement. Partial/retracted devices refuse new admissions. Existing
climbers/crossers keep their leases and finish safely; obsolete waiting work is
released by the existing local discovery/planning cancellation boundary.

Pending safe-retract intent survives its failed operation. After restoration it
resumes only when request and occupant leases have drained. Fresh extension
commands supersede that retract intent under the existing safety rules.

**Initially Broken** is an authored, paused-only edit with document history.
**Live Broken** is a runtime-only edit, available paused or running. YAML and
binary World schema 35 store only initial state; older Worlds without the field
default to working. Settings replay and reset restore authored condition and
extension. Reset clears Agent condition memories.

Agents observe devices from their existing approach/Location boundary, including
idle/passing observers and both Ladder landings. Each remembers Broken status
and extension percentage until re-observation or reset. Remote route queries use
that memory or baseline expectations, never remote live condition/extension.
Remembered fully extended Broken devices need no preparation and remain subject
to authored approach-side Permission adherence. Memories are individual; remote
failures and repairs do not notify Agents or expire their knowledge.

Unusable current Paths require Route planning, bypass Route persistence, and
produce Route loss when no replacement exists. Usable changes/restoration use
voluntary planning and normal Route persistence. No repair-checking trip is
created after Route loss. Admitted traversal is never interrupted.

## Headless coverage

- `pf-smoke-simulation --check brokenExtensibles/<name>`: `frozenMotionAndAdmission`,
  `waitingOperationsAndSafety`, `individualLocalMemory`, `planningAndPersistence`,
  and `safeRetractionAndPermission` (Room/Transit Ladders, Bridges, concurrent
  crossings, safe retract resumption, and eager/value-capture agreement).
- `pf-smoke-persistence --check extensibleBrokenLifecycle`: authored/live split,
  YAML/binary, legacy/schema rejection, settings replay, reset, exclusions, demo.
- `pf-smoke-editor --check extensibles/controlsAndHistory`: real CPU-only ImGui
  controls, paused/running scope balance, status, undo/redo, excluded devices.
- `pf-smoke-render --check extensibles/warningPreservesPosition`: actual Solid
  and Wireframe canvas output, warning placement and unchanged physical geometry.

All checks are owned by the existing domain smoke modules, require no window,
GPU, display server, clipboard service, or dialog, and are included in CTest.
