# Fixed inset physical controls (#433)

This slice implements the Bulkhead Door, Airlock, and Force Bridge placement
rules in [#425](https://github.com/ajare/promethium-fermide/issues/425).
It does not migrate Dumbwaiters or add object-specific Button artwork.

- At Bulkhead threshold `B`, required left/right controls belong respectively
  to cells `B-1` / `B`, at offsets `0.75` / `0.25`. Authored control demand and
  approach-specific permissions are preserved.
- An Airlock has one control outside each entrance, at the same inset offsets.
  Its owned Bulkhead Doors generate no independent controls. Security Scanners
  and Decontamination Chambers remain automatic and buttonless.
- For a Force Bridge spanning `L…R`, controls belong to permanent support cells
  `L-1` / `R+1`, at offsets `0.75` / `0.25`. Authored count and selected endpoint
  remain authoritative. There is no span host or opposite-end fallback.
  A blocking boundary between support and span invalidates that endpoint;
  walls beyond support do not invalidate its inset.

`World::insetControlDemand` supplies authored geometry/roles to the canonical
allocator. Validity, reflow, transactional reconstruction, and the existing
up-to-four same-Location stacking policy are shared with migrated Door-rule
controls. Cell-side registrations are bookkeeping, not physical positions:
additional controls in one cell may have distinct quarter-cell centres without
forming a vertical stack or sharing a graph approach. Each Button retains its
own Interaction point, commands, and authorization.

Bulkhead graph crossings are connected explicitly, rather than relying on row
adjacency: inset controls can sort between threshold endpoints. Ordinary row
edges must not bypass that controlled crossing. Runtime opening and extension
do not recalculate authored placement.

No document schema change is required. Load, construction replay, supported
edits, clipboard placement, and undo/redo derive the new positions from authored
owners. Binary readers for Bulkhead and Force Bridge control permission IDs
match the existing uint64 writers and validate the persisted ID range.

## Headless coverage

- World `two-sided-buttons`: fixed candidates, asymmetric demand, one-cell
  walled hosts, independent permissions, YAML/binary replay, graph approaches,
  targeting, distinct inset/centred controls in one cell, support refusal,
  transactional invalid load, and runtime Bulkhead/bridge operations.
- World Airlock checks: approved outside insets and coexistence with light
  switches; existing ownership, structural edit, and refusal coverage remains.
- Editor Airlock, Bulkhead Door, and extensible checks: creation/configuration,
  move/resize where supported, clipboard API mirrors, refusal, document history,
  undo and redo. Airlock clipboard copying is not an existing editor capability.
- Render `backButtonRendersAsOutlineOnly`: production draw-command geometry and
  hit targeting at the three owners' approved positions.
- Existing Permissions, Transports, Simulation, and Persistence modules protect
  operation-specific authorization, preparation sides, controlled Bulkhead
  routing, Airlock journeys, and automatic Chamber journeys.
