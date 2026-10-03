# Security scanner authoring (#339)

The palette's **Scanner** tool creates a distinct same-Layer Security scanner.
Drag horizontally across empty whole cells between adjoining walkable Room or
Corridor ends. The complete requested footprint must be free; it is never carved
or shortened. Drag direction fixes entry and exit, including drags within a single
cell. The selected chamber shows direction, capacity **1** regardless of length,
1-second pre-delay, 2-second complete scan, 1-second post-pause, and a 0.5-unit
sensor distance. The canvas displays an arrow and capacity.

This authored slice is deliberately **non-traversable**. Both generated Bulkhead
Doors remain closed; no buttons, Interaction points, or journey resource are
created. Door motion/configuration and independent object edits are refused.
Agent creation, placement, and relocation inside are refused. Move, resize,
delete, and reversal are not offered or supported for the chamber yet. Automated
journeys and structural editing belong to follow-up tickets, not #339.

World schema **43** persists `securityScanner` construction records: Layer,
Level, horizontal geometry, direction, fixed capacity/configuration, and original
adjoining wall states. Door ownership and left/right relationships are canonical:
replay generates exactly two protected shared Bulkhead Doors from that record,
never separate ordinary Door records. YAML and binary documents use the same
validated schema; older scanner-free Worlds remain readable. Invalid geometry,
configuration, missing fields, wall-restoration contradictions, and scanners in
older schemas are rejected transactionally. Configuration is fixed to defaults
in this slice; unsupported non-default document values are rejected, not dropped.

Creation uses normal document snapshots/history. Undo removes the chamber and
both shared Doors and restores original wall states; redo reconstructs the same
configuration and ownership. Existing detached open-wall restoration records
remain supported. Reset and unrelated structural replay reconstruct closed,
empty authored chambers.

Headless modular checks:

- `pf-smoke-world --check securityScanners/chambers`
- `pf-smoke-world --check securityScanners/preflight`
- `pf-smoke-persistence --check securityScanners/authoredRoundTripAndReplay`
- `pf-smoke-editor --check securityScanners/editorCommandsAndHistory`
- `pf-smoke-render --check securityScanners/chamberCommandStream`

Checks exercise production World and palette commands, real Selection readouts,
undo/redo, rendering command streams, both serialization formats, malformed-load
rollback, reset, replay, and Layer compaction. They require no OS windows, dialogs,
clipboard access, or manual interaction. Airlock and Bulkhead checks retain their
existing owners. See [Linux validation](linux-smoke-validation.md).
