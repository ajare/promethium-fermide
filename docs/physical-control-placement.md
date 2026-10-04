# Physical-control placement boundary (#426)

`core/PhysicalControlPlacement.h` separates value-only candidate generation and
allocation from World application. `WorldPhysicalControls.cpp` owns creation,
cell registration, Button geometry and interaction-position updates. Device
bindings, operation-specific permissions and traversal resources are unchanged.

- `legacyCandidates` adapts the existing cell/left-middle-right convention,
  including its alternate side. `allocateLegacy` returns assignments without
  mutating demands or World objects. World applies them only after the row has
  been allocated successfully.
- `Candidate::explicitHost` carries a host cell, a centre offset in quarters
  (`0..3`), and a separate legacy cell-side registration. An offset of `1.0`
  must instead use the next cell at `0.0`. Registrations remain legacy slots;
  this is not a new capacity/stacking policy.
- Every owner can pass a `Demand` through the same private World creation
  overload. Its optional `Owner` contains the fixed owner type, authored
  footprint, hosting Location geometry, and control role/Stop/doorway position.
  None of these fields contains a runtime ID. Unmigrated callers retain the
  original overload; their demands explicitly have no owner key. Location
  light switches exercise the explicit-host overload at their unchanged centre.
- Ordinary Doors, transport doorways, Dumbwaiters, bridges, Ladders, Platform
  lifts, Bulkheads and Airlocks can migrate separately. A transport doorway's
  owner key must describe its transport, not the generated Door. Endpoint keys
  describe the whole owning footprint, not the selected Button cell. Invisible
  panels/selectors are not physical demands. In this source state Dumbwaiter
  landings still use direct Interaction points, not Button SectorObjects; this
  refactor does not convert them.

The allocator deliberately preserves the previous component order, preference
for maximum separated centres, current-assignment retention, default-side tie
breaks, and coincident-border horizontal nudges. It does **not** use Owner keys
for sorting, change candidate validity, allocate across Locations, introduce
vertical stacks, change transactional editing rules, or implement #425's new
placement policy. Those changes belong to the subsequent tickets.

Coverage extends the existing World two-sided Button check with legacy
preferred/fallback positions for one- and two-cell Doors, production interaction
positions and save/load geometry. Editor document history checks exact ordinary Door Button
and approach reconstruction through undo/redo alongside Dumbwaiter edits. Existing Render, permissions,
graph, persistence, authored-workflow and simulation suites remain the contracts
for the unchanged production behavior.
