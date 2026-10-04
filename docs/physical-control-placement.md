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
Transport-owned Door controls were migrated separately in #431 (see below).

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

Collision-free assignments always win. If separation is impossible, up to four migrated
controls may share a centre only within the same hosting Location (#430).
Only feasible assignments are compared: all candidates must be valid, coincident
controls must share a Location, and every stack must fit within four members.
An over-capacity search branch is skipped, not treated as refusal of the edit.
The allocator minimises total buttons above bottoms, then maximum stack size,
then non-preferred assignments, then canonical preference ties. Adding a member
to an occupied position contributes one above-bottom Button, not one per pair.
Disconnected conflict components retain their best assignment at each capacity;
the smallest common capacity attaining the minimum total above-bottom count is
selected before preference optimisation. This avoids unnecessarily sacrificing
preferred sides in a component when another component already sets the maximum.
Indistinguishable definitions, cross-Location coincidence, invalid hosts, and
unavoidable stacks of five or more remain atomic refusals. Legacy-owner migration
belongs to subsequent tickets, not this slice.

The bottom Button stays at normal height; each subsequent member is one
Button-height plus a 25%-height gap above its predecessor, at identical centre X. Cell registrations retain one
representative plus independently owned additional members. Graph construction
maps all member SectorObjects and their individual vertex identifiers to exactly one
normal-height approach. Visible geometry is not interaction reach geometry:
all Interaction points keep their own identity, bindings, eligibility and
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
clipboard/history reconstruction, unstacking, and transactional refusal
through authoring and invalid loading. #430 extends those production seams to
three/four members, each independently authorized and routed without upward
Agent movement, and tests two-pair spreading and preference/canonical ties after
wall removal. A value-only exhaustive oracle checks 7,776 five-control candidate
layouts against all four optimisation tiers, including capacity and Location
filtering; explicit cases cover simultaneous side changes, a feasible alternative
to an explored fifth member, unavoidable fifth-member refusal, and the global
maximum across disconnected conflict components. This allocator coverage does
not migrate the blocked legacy owner types.

## Lift and Shuttle landing controls (#431)

Passenger Lift and Shuttle landing calls now participate in the same complete
allocator as ordinary Doors and light switches. Every required doorway prefers
its `X+W` host at offset `0.0`, with only `X` at offset `0.0` as fallback. Lift
Doorways use the full one/two-cell shaft width; each currently supported Shuttle
Doorway is one cell wide, regardless of carriage width. Hosting Location,
walkable support, shared walls and Bulkhead thresholds are checked independently
at every landing. Even a full-Location doorway can use its left host when there
is no retained shared wall there; no extra outside space is required.

Canonical ownership is the Lift shaft or Shuttle track's authored geometry and
type, not a generated Door, vehicle position or allocation ID. Roles retain the
authored doorway X and Level, so Stop/doorway ordering is spatial. Distinct calls,
landing permissions and traversal relationships remain unchanged; invisible
onboard selectors produce no physical demand. Transport motion and opening do
not reflow landing controls.

Creation preflights all new landing demands jointly with existing controls before
beginning a structural edit or constructing the transport. Shuttle doorway floor
and occupancy checks also precede mutation. Existing detached construction
validation/replay handles supported transport movement, Stop/doorway edits,
removal, structural reconciliation, load and document history; final placement is
recomputed rather than serialized. Protected landing requirements now read their
existing uint64 binary wire representation before validated narrowing, matching
the writer (including reset/replay snapshots).

World coverage exercises independent multi-Stop and full-width Lift candidates,
wide/multiple Shuttle doorway calls with complete side reassignment, independent
hosting Locations, mixed protected stacks in opposite creation orders, transport
geometry-based ordering, graph/interaction/visible targeting, YAML/binary replay,
invalid creation/movement no-ops, selected Agent calls and occupied Lift journeys,
and deletion unstacking. Editor document history verifies transport movement
undo/redo reconstructing and removing mixed stacks; production draw commands and
hit testing cover mixed Lift and Shuttle stacks. Existing transport authoring,
permission, admission and clipboard/reconstruction contracts remain in the final
validation matrix; no new unsupported transport clipboard capability is added.
