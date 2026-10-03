# Security scanner chambers (#339, #340, #341, #342, #343)

The palette's **Scanner** tool creates a distinct same-Layer Security scanner.
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
Direct Agent creation/placement/relocation inside is refused. Scanner move,
resize, delete, and reversal remain unsupported, including while occupied.

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

Interrupted occupied-journey recovery and structural editing remain separate
follow-up tickets.

## Persistence and history

World schema **43** persists `securityScanner` construction records: Layer,
Level, geometry, direction, authored sensing/timing, fixed capacity, and original adjoining wall
states. Replay generates exactly two protected shared Bulkhead Doors from that
record, never separate ordinary Door records. YAML and binary documents use the
same validated schema; older scanner-free Worlds remain readable. Invalid
geometry/configuration, missing fields, wall-restoration contradictions, and
scanners in older schemas are rejected transactionally. Non-default sensing/timing is retained by both formats and replay; unsupported
configuration is rejected rather than silently dropped.

Creation uses normal document snapshots/history. Undo removes the chamber and
both Doors and restores original wall states; redo reconstructs configuration
and ownership. Detached open-wall restoration records remain supported.

## Headless checks

- `pf-smoke-world --check securityScanners/chambers`
- `pf-smoke-world --check securityScanners/preflight`
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
