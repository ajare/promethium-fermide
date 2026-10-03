# Security scanner chambers (#339, #340)

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
Another Agent waits outside rather than piggybacking. After crossing the fully
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
This is the stable seam for later beam graphics and timing configuration. Route
duration estimates include automatic opening, entry closure, and the four-second
sequence without fictitious button motion or interaction costs.

Global pause freezes motion, timers, and progress. Reset and document load replay
closed, empty chambers, clear queues/reservations/operations, and restore Agents'
authored positions and Paths through the existing World restoration pipeline.
Generated Doors cannot be independently operated, configured, moved, or deleted.
Direct Agent creation/placement/relocation inside is refused. Scanner move,
resize, delete, and reversal remain unsupported, including while occupied.

Beam graphics, fair admission policy, configurable sensing/timing, interrupted
journey recovery, and structural editing are separate follow-up tickets.

## Persistence and history

World schema **43** persists `securityScanner` construction records: Layer,
Level, geometry, direction, fixed defaults/capacity, and original adjoining wall
states. Replay generates exactly two protected shared Bulkhead Doors from that
record, never separate ordinary Door records. YAML and binary documents use the
same validated schema; older scanner-free Worlds remain readable. Invalid
geometry/configuration, missing fields, wall-restoration contradictions, and
scanners in older schemas are rejected transactionally. Unsupported non-default
configuration is rejected rather than silently dropped.

Creation uses normal document snapshots/history. Undo removes the chamber and
both Doors and restores original wall states; redo reconstructs configuration
and ownership. Detached open-wall restoration records remain supported.

## Headless checks

- `pf-smoke-world --check securityScanners/chambers`
- `pf-smoke-world --check securityScanners/preflight`
- `pf-smoke-simulation --check securityScanners/automaticJourneys`
- `pf-smoke-simulation --check securityScanners/presenceAndEmptyTimeout`
- `pf-smoke-simulation --check securityScanners/destinationAndMobilityGates`
- `pf-smoke-simulation --check securityScanners/resetAndLoad`
- `pf-smoke-persistence --check securityScanners/authoredRoundTripAndReplay`
- `pf-smoke-editor --check securityScanners/editorCommandsAndHistory`
- `pf-smoke-render --check securityScanners/chamberCommandStream`

Checks exercise World commands, real Paths, fixed ticks, snapshots, Selection
readouts, rendering command streams, undo/redo, YAML/binary load, reset, replay,
and Layer compaction. They require no windows, dialogs, clipboard, private-state
injection, or new harness. Airlock/Bulkhead regressions retain their existing
owners. See [Linux validation](linux-smoke-validation.md).
