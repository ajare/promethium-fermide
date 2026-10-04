# Physical-control placement boundary (#426) and wall-safe authoring (#427)

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

For unmigrated owners, the allocator deliberately preserves the previous component order, preference
for maximum separated centres, current-assignment retention, default-side tie
breaks, and coincident-border horizontal nudges. It does **not** use Owner keys
for sorting, change candidate validity, allocate across Locations, introduce
vertical stacks, change transactional editing rules, or implement #425's canonical allocation/stacking policy. Those changes belong
to the subsequent tickets.

## Ordinary Doors and light switches (#427)

Ordinary Door controls now use only `X+W` at offset `0.0` (preferred), or `X`
at offset `0.0`, independently in each required approach Location. The registered
cell is the explicit host, not the preceding Door cell. Location light switches
remain at their authored cell, offset `0.5`, on the Location's base Level.
Transport-owned Door controls still use legacy placement.

`validPhysicalControlDemand` is the common authored host/support and boundary
policy. Hosts must belong to the required Room, Corridor or Facade and have usable
floor. Offset-zero candidates cannot cross retained shared Sector walls or a
Bulkhead threshold; removed shared walls and outer boundaries do not block them,
but they never waive host/support requirements. Bulkhead runtime opening is not
an input. Centred switches are unaffected by cell-end walls and never relocate.

World preflights creation/control additions, shared-wall restoration, incoming
Location/Background boundaries and Walkway removal without touching live controls.
Movement, resize, deletion and configuration use the existing detached construction
replay validation. Replay/load defer wall/support filtering until the authored
layout is complete; `finishBuild` plans all rows before applying placements and
building the graph. Document restoration and clipboard Door paste use those same
World authoring/replay seams. Impossible required controls are refused, not omitted.

This slice retains the legacy row allocator for existing conflict handling; it does
not introduce canonical multi-control allocation or stacks. Migrated controls do
not receive legacy coincident-border nudges, and coincident explicit assignments
are refused. Production Button geometry, graph approaches and Interaction point
positions share the selected centre; hit testing includes the preceding-cell half
of an offset-zero shape while returning its actual host SectorObject. Bindings,
operation-specific permissions and runtime device behavior are unchanged.

Coverage includes one/wide Doors, independent approaches, Room/Corridor/Facade
hosts, retained/removed walls, outer-boundary host failure, missing support,
open/closed Bulkhead thresholds, atomic authoring/wall/support/load refusals,
move/resize, YAML replay, document undo/redo, draw commands and hit testing.
Existing permission, routing and runtime checks remain the operation contracts.

## Preparatory refactor coverage

Coverage extends the existing World two-sided Button check with legacy
preferred/fallback positions for one- and two-cell Doors, production interaction
positions and save/load geometry. Editor document history checks exact ordinary Door Button
and approach reconstruction through undo/redo alongside Dumbwaiter edits. Existing Render, permissions,
graph, persistence, authored-workflow and simulation suites remain the contracts
for the unchanged production behavior.

## Canonical allocation and shared approaches (#428, #429)

The sections above describe the earlier slices. Migrated ordinary Door controls
and Location light switches now allocate jointly across each Layer/Level. The
complete authored canonical tuple orders preference ties and stack members;
previous placement and runtime identifiers are never allocation inputs.

Collision-free assignments always win. If separation is impossible, two migrated
controls may share a centre only within the same hosting Location. The allocator
minimises buttons above bottoms, then non-preferred assignments (with capacity
two, maximum stack size is already determined), then canonical preference ties.
Indistinguishable definitions, cross-Location coincidence, and unavoidable stacks
of three or more remain atomic refusals. Legacy-owner migration and larger stacks
belong to subsequent tickets, not this slice.

The lower Button stays at normal height; the upper is one Button-height plus a
25%-height gap above it, at identical centre X. Cell registrations retain one
representative plus independently owned additional members. Graph construction
maps both SectorObjects and their individual vertex identifiers to exactly one
normal-height approach. Visible geometry is not interaction reach geometry:
both Interaction points keep their own identity, bindings, eligibility and
operation-specific requirements at the walkable position. Explicit Agent requests
and traversal intent select a control, never every control at the shared vertex.
The editor and runtime mouse targeting use each Button's separate visible shape.

Deletion, newly supported/available sides, document replay, clipboard authoring,
and history restoration recompute placement rather than retaining stacks. No
stack geometry or shared-vertex identity is serialized. Door control permissions
read their existing uint64 wire representation before validated narrowing, allowing
protected stacks to round-trip through binary as well as YAML documents.

Headless coverage in `world/TwoSidedButtons.cpp`, `editor/DoorPanel.cpp`, and
`render/DoorButtons.cpp` exercises both protected commands, unauthorized refusals,
Agent approach and Door crossings without upward movement, canonical equal-X
ordering, one graph vertex and both lookup kinds, independent hit selection,
production draw styles/spacing, creation-order independence, YAML/binary replay,
clipboard/history reconstruction, unstacking, and transactional triple refusal
through authoring and invalid loading.
