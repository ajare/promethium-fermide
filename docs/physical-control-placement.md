# Physical-control placement

The authoritative placement specification is [#425](https://github.com/ajare/promethium-fermide/issues/425).
All stationary physical Buttons use one canonical policy. Invisible standalone
BoothWindow back-side panels and invisible onboard destination selectors do not
participate; automatic Chambers and non-extensible Ladders gain no controls.

## Policy and application

`core/PhysicalControlPlacement.h` contains value-only authored owner keys,
explicit host/quarter-cell candidates and the complete canonical allocator.
`WorldPhysicalControls.cpp` owns candidate validity, preflight, host registration,
Button geometry and interaction-position application. No ownerless adapter,
legacy allocator, previous-placement preference, side-slot uniqueness rule,
edge nudge or alternate vertical collision policy remains (#436).

An owner key contains the fixed type, full authored footprint, hosting Location
geometry and control role/Stop/doorway position. Neither runtime IDs, current
Button positions nor device motion are policy inputs. Transport doorway keys
identify the transport, not its generated Door. Endpoint keys describe the full
owning footprint, not the chosen control host.

- Ordinary Doors and Lift/Shuttle landing doorways prefer `X+W` at offset `0.0`,
  falling back only to `X` at offset `0.0`, independently in each approach Location.
- Dumbwaiter landings and extensible Ladder endpoints independently prefer
  `X+1` at offset `0.0`, otherwise `X` at offset `0.0`.
- Platform lift Stops independently prefer `X+W` at offset `0.0`, otherwise `X`
  at offset `0.0`. Their adjoining support must be permanent; no common-side
  restriction remains. Queue approaches depend on authored support, not motion
  or conflict-driven Button assignment.
- Bulkhead and Airlock outside controls use their fixed inset hosts/offsets;
  Airlock-owned Bulkheads generate no additional controls.
- Force Bridge controls remain on their selected permanent endpoint supports,
  at the specified insets, never over the retractable span or at another endpoint.
- Location light switches remain on the authored host at offset `0.5`.

The shared host/support and boundary filter requires the intended Location and
walkable support. Ladder endpoints, Dumbwaiter apertures/control hosts, Platform
approaches and Force Bridge supports require permanent Ground/Walkway support.
Offset-zero shapes cannot cross retained shared walls or Bulkhead thresholds.
Removed walls and outer boundaries do not block shapes, but never waive host,
support or approach-Location requirements. Insets and centred switches do not
straddle cell ends; Force Bridges additionally require the connection between
support and span to be unobstructed. Runtime opening does not affect validity.

## Assignment, stacks and interaction

Allocate together across each Layer/Level, regardless of Sector ownership.
Filter feasible assignments first: every demand has a valid candidate, coincident
controls belong to one Location, and each stack contains at most four Buttons.
Indistinguishable duplicate definitions are invalid. Among feasible assignments,
minimise total members above bottoms, then maximum stack size, then non-preferred
placements, then the approved canonical authored tuple's preference ties.
Disconnected conflict components retain their best solution at each capacity
before choosing the globally best maximum. Allocation never mutates its inputs.

The bottom Button retains normal height. Subsequent members have one
Button-height plus a 25%-height gap between them, at the same centre X. Host cells
register every member in a vector, not three side slots or separate overflow
registrations. Graph construction resolves all coincident same-Location members
and their individual vertex identifiers to one normal-height approach. Every
Button retains its own Interaction point, commands, permission requirement and
operation semantics. Visible height never raises interaction reach or the Agent.
Production rendering and mouse targeting use the individual visible shapes,
including the preceding-cell half of an offset-zero Button.

This preserves ADR 0001's device/traversal separation and ADR 0015's independent
operation authorization. Sharing an approach never combines operations or grants
another Button's permissions.

## Authoring and reconstruction

Creation, supported geometry/control/support/wall edits, deletion, document
loading, construction replay, clipboard authoring and undo/redo all submit the
same authored demands and use the same canonical assignment/application path.
No Button placement, stack rank or shared vertex is serialized. Replay defers
wall/support filtering until the authored layout is complete; `finishBuild`
preflights every row before applying placements and building the graph. Detached
replay validates impossible structural edits and documents before live mutation.
Wall edits plan the complete row once. Adding Walkway support validates changed
Ladder endpoints and Platform Stops before beginning a live structural edit.
Refusal preserves the prior layout, graph, operations, document and history.

Requirements for physical controls persist against authored owner/role records,
not redundant replay-order Interaction point handles. Light switches now carry
an optional `permissionRequirement` array in their existing named-field record;
its absence means unrestricted. Existing schema-50 wire fields and numeric
permission representations remain compatible. The legacy top-level requirements
array is still read and validated, then migrated through the production
permission-authoring seam onto owner/role records. Airlock's deliberately skipped
old internal-control IDs remain skipped. Dumbwaiter stale-handle non-reuse remains
in force; legacy handles are translated to the live reconstruction base after
validation. Landing-permission registrations track the actual replay record
ordinal, so edits after load/replay cannot update an unrelated construction record.
Invisible panels/selectors retain their existing ownership and authorization.
Legacy Marker removals still resolve shifted Dumbwaiter/Furniture child slots by
stable Marker identity. No compatibility handling selects a different allocator.

## Headless coverage

`world/TwoSidedButtons.cpp` covers every physical owner family, canonical
creation-order independence, shared approaches, selected protected operations,
wall/support/control edits, runtime invariance, YAML/binary round trips and
transactional refusals. Its all-owner fixture checks independent requirements
through repeated reset/replay and subsequent control edits, historical document
migration, and mixed YAML/binary documents with each owner family's invalid
geometry. The exhaustive oracle compares 7,776 candidate layouts against all four
optimisation tiers, Location/capacity constraints and simultaneous side changes.

Editor Door, extensible, Airlock and BoothWindow/Dumbwaiter checks exercise actual
snapshots, clipboard/readback authoring and undo/redo. Mixed Dumbwaiter clipboard
history also preserves an unrelated protected light switch and rejects stale
handles. Persistence includes legacy handle-only Airlock/light requirements and
skipped-ID compatibility. Render Button checks inspect production draw commands,
stack spacing and independent hit selection; permissions, routing, transports and
simulation retain the selected-command/Agent contracts. All checks are headless.

Related device contracts: [fixed inset controls](fixed-inset-controls.md) and
[Dumbwaiters](dumbwaiters.md). This completes allocation/reconstruction migration,
not the blocked final feature-wide follow-up or unspecified Button artwork.
