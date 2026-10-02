# Bulkhead Doors: Broken same-Layer passage (#315)

Standalone Bulkhead Doors use the ordinary Door Broken contract. Breakage freezes
physical position: only 100%-open thresholds admit new crossings, including a
Door whose closing animation has just been requested. Admitted crossings finish
and release their permits and leases. Fully open passage remains concurrent and
bidirectional. Automatic presence sensors, manual activation and remote controls
cannot operate a Broken Bulkhead Door; pending operations fail. Restoration
allows fresh requests and resumes the existing obstruction/lease safety rules.

Selection offers paused-only **Initially Broken** authoring and separate
**Live Broken** break/restore controls, available while running or paused. Live
edits do not dirty or change the World document. Reset restores authored condition
and clears Agent memories. YAML and binary World schema 34 persist the initial
toggle; older Worlds without it remain working. Copy/paste and paused settings
edits retain the authored toggle. Status shows both Broken and physical position;
the canvas warning sits above the physical leaf, in solid and wireframe views.

Agents observe from either adjacent Location, including idle bystanders and
Agents passing without a Path through the Bulkhead Door. Their remembered Broken
status and physical condition persist until local re-observation or reset. Search
uses this knowledge, not remote live state. An unusable current Path requires
mandatory Route planning and Route loss if no alternative exists. Restoration
and usable condition changes use voluntary planning and Route persistence. There
is no memory expiry, remote notification or deliberate repair-checking trip.

## Demo

Open `resources/test-worlds/broken-bulkhead-doors.world.yaml` in the editor:

1. Resume: the local observer uses the ordinary Door detour around the Broken
   automatic Bulkhead Door. The passing observer enters the Right hall and learns
   its condition even though its Path does not use that threshold.
2. Select the Bulkhead Door and clear **Live Broken**. Agents in either adjacent
   hall learn restoration locally; presence inside sensor distance can open it.
3. Break it while opening: its percentage freezes and new crossings fail. Restore,
   allow it to open fully, then break again: the physical opening stays passable.
4. Reset: authored Broken/closed state returns and memories are cleared. Paused
   **Initially Broken** edits are undoable and saved independently of live edits.

## Headless verification

- `pf-smoke-simulation --check brokenBulkheadDoors/<check>`: frozen position,
  commands, automatic bystanders on both sides, pending/fresh operations,
  admitted crossings, individual/stale memories, routing and planning.
- `pf-smoke-persistence --check bulkheadDoorBrokenLifecycle`: YAML/binary,
  legacy/defaults, authored/live separation, both authoring orientations, reset
  and demo journeys.
- `pf-smoke-editor --check doorpanel/checkBulkheadBrokenControlsAndHistory`:
  production shared Selection condition controls, status and undo/redo, CPU-only.
- `pf-smoke-render --check bulkhead/checkBrokenWarningPreservesPosition`:
  production canvas solid/wireframe warning and unchanged physical leaf geometry.

Airlock cycles, other moving device slices, random failures, independently broken
buttons and Agent-behaviour-driven repair/breakage are outside this ticket.
