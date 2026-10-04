# Security scanner chambers (#339, #340, #341, #342, #343, #344, #345)

The palette's **Chamber** tool creates a same-Layer Chamber with the Security Scanner subtype through World-owned Chamber commands. Selection identifies the Sector as Chamber and offers a required **Subtype** dropdown containing **Security Scanner** and **Decontamination Chamber**. Choosing the active subtype is a no-op: configuration, dirty state, and document history remain unchanged. Airlock remains separate and unchanged.
Drag horizontally across empty whole cells between adjoining walkable Room or
Corridor ends. The complete requested footprint must be free; it is never carved
or shortened. Drag direction fixes entry and exit, including single-cell drags.
Capacity is **1**, regardless of length. Defaults are a 1-second pre-delay,
2-second successful scan, 1-second post-pause, and 0.5-unit entry sensor distance.

## Automatic journey

Entry presence sensing measures the physical gap from the Door's edge to the
Agent's nearest edge on the threshold row, independently of Path intent. An
empty presence-triggered opening times out after the normal 5-second open hold
and closes without scanning. Only forward routes are feasible. The exit cannot
admit reverse traffic, even while open, and ordinary Bulkhead edges cannot bypass
either threshold. Destination Location requirements and Door Mobility use apply
before admission; Buttons capability and scanner-specific permissions do not.

The World-owned Traversal resource reserves its sole slot before admission.
The oldest eligible waiting ticket receives that exclusive reservation; eligibility
requires an active entry-side Agent within sensor range, usable Door Mobility,
and destination Location access. Another Agent waits outside rather than
piggybacking, even during a presence-only opening or in a longer chamber.
Cancellation before crossing and deactivation before admission release claims;
abandoned boarding cannot hold the slot indefinitely. Movement cancellation
preserves a crossing already in flight, while deactivation releases an
uncommitted boarder at its safe source boundary. After crossing the fully
open entry, the occupant walks to the centre and remains there. Entry closure
starts only once positioned and clear of crossing. When both Doors are fully
closed, the coordinator starts **1s pre-delay → 2s scan → 1s post-pause**, then
opens the exit automatically. The occupant disembarks through the fully open
exit, which closes before entry can reopen. At every tick at least one Door is
fully closed. Only the chamber coordinator advances its two Doors; shared Sector
object updates do not advance them again.

Selection exposes phase, remaining timed-phase duration, direction, occupancy,
and normalized scan progress. The canvas shows phase/countdown, direction, and
capacity. Progress is zero before scanning, increases deterministically from
zero to one during the scan, and stays one through exit until the next opening.
This is the stable seam for beam graphics and later timing configuration. Route
duration estimates include automatic opening, entry closure, and the four-second
sequence without fictitious button motion or interaction costs.

Global pause freezes motion, timers, and progress. Reset and document load replay
closed, empty chambers, clear queues/reservations/operations, and restore Agents'
authored positions and Paths through the existing World restoration pipeline.
Generated Doors cannot be independently operated, configured, moved, or deleted.
Direct Agent creation/placement/relocation inside is refused. Structural edits
require a globally paused World and an empty chamber with no threshold crossing.

## Scan beams

During Scanning only, the canvas draws two simultaneous translucent red beams
above the occupant: a full-width horizontal beam sweeps ceiling → floor → ceiling,
and a full-height vertical beam sweeps left → right → left. Both complete one
out-and-back sweep over the scan duration, independent of travel direction and
chamber width (including one cell). Positions consume only public normalized scan
progress; there is no renderer clock. Pause freezes the beams, and reset/load
removes them. Pre-delay, post-pause, and all other phases show no beams.

Physical multiple occupancy defensively latches an **Occupancy violation: multiple
Agents** phase, clears scan timing/progress, and stops Door motion and further
admission until reset. Selection and snapshots report the phase; it is never a
successful multi-Agent scan. Public placement still refuses scanner occupants.

## Authored sensing and timing

While globally paused, Selection edits sensor distance, pre-scan delay, complete
out-and-back scan duration, and post-scan pause through the World configuration
command and normal document history. Sensor distance is finite and non-negative,
with no fixed upper bound; pre/post pauses are in [0,10] seconds and scan duration
in [0.1,10] seconds. Defaults remain 0.5 units and 1s/2s/1s. Invalid command or
loaded values are rejected atomically.

The sequence snapshots all three timings when entry opening begins. Edits during
an occupied global pause apply to the next sequence, not the active countdown or
progress. Durations round upward to fixed ticks; zero pre/post delays skip their
phases without an intentional pause. Public scan progress uses the effective
active duration, so beam consumers need no separate clock. Route estimates use
the authored timings and automatic Door motion, with no Airlock cycle or Buttons
assumptions.

## Interrupted journeys

Admission commits the forward exit independently of destination intent and Path
lifetime. Destination replacements defer Route planning until the occupant leaves;
Path loss restores only the committed forward chamber suffix, never an entry-side
return. Clearing an authored Path through the World command likewise retains the
occupied slot until exit. Location grant loss or requirement tightening after
admission cannot revoke that exit; before admission they refuse/reconsider entry.

Deactivation freezes Agent movement, not automatic Door motion or scan timing.
A centred inactive occupant may finish scanning and await the open exit, retaining
capacity until reactivation and completed disembarkation. An inactive occupant
still positioning waits for reactivation before entry closure. Global pause freezes
both movement and device operation. Scanner waiting tickets and selected
reservations survive pause/resume without reordering or duplication; cancellation
and ineligible pre-entry changes retire claims through normal traversal cleanup.

Perceived route costs retain the automated-duration estimate. Only an entry-local
Route observation adds that approach's queue service estimate and density; remote
live occupancy, phase, and queues are not revealed. No fictitious Buttons
interaction, scanner control permission, or new Mobility category is introduced.
Structural edits preserve committed journeys by refusing occupied or crossing chambers.

## Safe structural editing

While paused, general Chamber controls edit horizontal position, Level, and width,
and **Delete Chamber** removes the selected Chamber. A separate **Security Scanner**
section exposes direction, sensor distance, all three timings, capacity, phase,
remaining time, occupancy, and scan progress. The canvas supports move/resize.
World `planResizeChamber`, `planRemoveChamber`, and `applyChamberEdit`
revalidate before mutation. Edits retain the same Layer,
positive whole-cell width, walkable Room/Corridor ends, fixed capacity one, and all
authored sensing/timing values. Reversal updates both routing and runtime gates.

Occupied chambers and crossings refuse move, resize, reversal, and deletion,
even while paused. Running or invalid edits change neither authored state nor
queues, walls, Doors, Paths, or history. Construction replay also refuses if
another Airlock or scanner has an occupant/crossing, rather than discarding its
journey. Valid empty edits retire old requests, reservations, and permits and
reconsider affected Paths while preserving Agent identity and destination intent.

Generated Doors move with the chamber and remain independently uneditable.
Old adjoining walls are restored exactly, including originally open walls;
deleting removes both Doors and the chamber's resource. Rendering, readouts, and
optional beams consume the unchanged public geometry/progress seams.

## Persistence and history

World schema **46** persists `chamber` construction records with required
`subtype: securityScanner`: Layer, Level, geometry, direction, authored
sensing/timing, fixed capacity, and original adjoining wall states. Legacy schema
43/44 `securityScanner` records migrate to that subtype without configuration loss.
Missing or unsupported Chamber subtypes are rejected before replay, leaving the
target World unchanged. Saving omits transient journey work and loading clears
it as before. Replay generates exactly two protected shared Bulkhead Doors from that
record, never separate ordinary Door records. YAML and binary documents use the
same validated schema; older scanner-free Worlds remain readable. Invalid
geometry/configuration, missing fields, wall-restoration contradictions, and
scanners in older schemas are rejected transactionally. Non-default sensing/timing is retained by both formats and replay; unsupported
configuration is rejected rather than silently dropped.

Creation, move, resize, reversal, and deletion use normal document snapshots/history. Undo removes the chamber and
both Doors and restores original wall states; redo reconstructs configuration
and ownership. Detached open-wall restoration records remain supported.

## Production interfaces

`World::canAddChamber` / `addChamber` support an explicit `ChamberSubtype`,
defaulting creation to `SecurityScanner`. `setChamberConfiguration`,
`planResizeChamber`, `planSetChamberSubtype`, `planRemoveChamber`, and `applyChamberEdit` retain the
existing paused, validated, transactional scanner edit semantics.
`ChamberTransit::getSubtype()` exposes the immutable subtype. Chamber is the sole
Sector and construction identity; the temporary SecurityScanner World interfaces
and Transit/Sector aliases have been removed. Legacy `securityScanner` document
recognition remains supported permanently and migrates to Chamber on load.
Scanner-specific phases, journey snapshots, configuration, and rendering retain
their behavior names; shared journey dispatch supports both Chamber subtypes. See [Decontamination Chambers](decontamination-chambers.md).

## Headless checks

- `pf-smoke-world --check securityScanners/chambers`
- `pf-smoke-world --check securityScanners/preflight`
- `pf-smoke-world --check securityScanners/structuralEdits`
- `pf-smoke-simulation --check securityScanners/editSafety`
- `pf-smoke-editor --check securityScanners/structuralHistory`
- `pf-smoke-editor --check securityScanners/selectionWorkflow`
- `pf-smoke-simulation --check securityScanners/committedInterruptions`
- `pf-smoke-simulation --check securityScanners/admissionAuthorizationChanges`
- `pf-smoke-simulation --check securityScanners/localRouteObservations`
- `pf-smoke-simulation --check securityScanners/automaticJourneys`
- `pf-smoke-simulation --check securityScanners/configuration`
- `pf-smoke-simulation --check securityScanners/contentionAndReuse`
- `pf-smoke-simulation --check securityScanners/abandonedAdmission`
- `pf-smoke-simulation --check securityScanners/defensiveOccupancy`
- `pf-smoke-simulation --check securityScanners/presenceAndEmptyTimeout`
- `pf-smoke-simulation --check securityScanners/destinationAndMobilityGates`
- `pf-smoke-simulation --check securityScanners/resetAndLoad`
- `pf-smoke-persistence --check securityScanners/authoredRoundTripAndReplay`
- `pf-smoke-editor --check securityScanners/editorCommandsAndHistory`
- `pf-smoke-render --check securityScanners/chamberCommandStream`
- `pf-smoke-render --check securityScanners/beamSweeps`

Checks exercise World commands, real Paths, fixed ticks, snapshots, Selection
readouts, rendering command streams, undo/redo, YAML/binary load, reset, replay,
and Layer compaction. Defensive occupancy coverage uses narrow test-only access
to the existing carried-Agent restoration seam (no public corruption API).
They require no windows, dialogs, clipboard, or new harness. Airlock/Bulkhead regressions retain their existing
owners. See [Linux validation](linux-smoke-validation.md).
