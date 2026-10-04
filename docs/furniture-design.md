# Furniture design interview

Status: agreed interview decisions, synthesised as a Furniture spec. The user approved the existing World/document, render-command, and editor-action testing seams. Feature implementation has not started.

## Agreed depth semantics

- Local depth is a non-negative integer within a Sector, distinct from the World's Layers. Depth 0 is front-most; larger values are farther back.
- Ordinary edges default to depth 0. In definitions, unassigned edge depth remains fixed at 0; explicitly assigned offsets resolve relative to the instance's depth. Placement rejects any resulting negative depth.
- Definitions author connections between fixed-depth-0 approaches and relative-depth routes; placement does not invent depth-changing connections.
- Numerical differences matter for route preference; depth is not physical distance.
- Route selection first minimises total Perceived route cost. Among equally costly complete Paths, it minimises the sum of absolute changes in Local depth, including the transition from the Agent's current edge.
- Entering a different Sector resets the depth-continuity baseline to 0 in that Sector. Depth numbers in different Sectors are not compared.
- A stationary Agent retains its incoming edge's depth for rendering and subsequent routing. An Agent without edge history starts at depth 0.
- Starting an edge immediately adopts that edge's integer render depth. Depth transitions are not interpolated and add no movement time or primary route-cost penalty; depth is not a physical third dimension.
- A waypoint at which a Path changes Local depth is non-skippable, even if it requires no interaction.
- Furniture edits that rebuild the graph preserve Agent physical positions and current Local depths. Invalidated Paths enter normal Route planning; Agents adopt the replacement edge's depth when traversal starts, subject to the Sector-entry reset rule.

## Agreed destination semantics

- Each usable Furniture point is a furniture-owned Marker with stable identity, selectable by Agent behaviours through the existing Marker destination model.
- A two-seat sofa has two selectable destinations. Optional side vertices are routing-only waypoints, not destination Markers.
- Usable Furniture Markers default to Blocks pathing: they are valid origins and destinations, but not intermediate waypoints.
- Reaching a Furniture Marker is ordinary Marker arrival. This feature adds no sitting state, exclusive seat occupancy, capacity, or reservations; multiple Agents may target the same usable point.

## Agreed definition and instance semantics

- Reusable Furniture definitions specify appearance, usable-point offsets, and optional side-route layout.
- Each Furniture instance supplies its Location, position, Local depth, and distinct usable-point Marker identities.
- Each instance has an editable name used to generate initial Marker names, such as `Lounge sofa / Seat 1`. Marker names remain individually editable; references use stable identities rather than names.
- Instances do not override the definition's layout initially.
- Furniture definitions and usable points have stable keys separate from display names. Renaming labels or reordering catalogue entries preserves an instance's existing usable-point Marker identities and behaviour references.
- Definitions explicitly list internal edges and their Local-depth offsets, and designate external connection points. The engine does not infer internal connectivity merely from vertex positions.
- Definitions live in a separate reusable Furniture catalogue rather than being embedded in each World.
- Catalogue files are manually authored for the first feature; no visual Furniture-definition designer is included. The editor manages instances.
- Loading fails with a clear diagnostic if a referenced catalogue or Furniture definition is missing; no placeholder or silent omission is used because Furniture affects routing.
- Worlds use current catalogue definitions on their next load, rather than saved layout snapshots. Stable usable-point keys preserve existing Marker identities. Invalidated placements or removed referenced usable points cause diagnostic load failure, not silent behaviour-reference changes.
- Live catalogue reload is out of scope initially.
- World catalogue references use a portable relative filename plus expected stable catalogue UUID, following the external-registry identity protection in ADR 0007. A mismatched catalogue identity is a load failure even if definition keys match.
- A definition's appearance is a list of World-tile-sized Image-set regions at integer tile offsets. One- and two-tile layouts are ordinary cases of that list; larger layouts use the same representation without scaling tiles.

## Agreed floor-routing semantics

- Where a Furniture definition supplies front and back routes, those explicit routes replace the ordinary floor connection across the Furniture's width; no depth-0 shortcut remains through that span.
- Furniture without side routes leaves ordinary floor routing unchanged.

## Agreed rendering semantics

- Render Local depths back-to-front (larger depths first).
- At equal Local depth, draw Furniture before Agents, so an Agent on the front route at the Furniture's depth appears in front of it. An Agent on a deeper route appears behind it.

## Agreed placement coordinates

- Furniture instances allow fractional x placement, but not fractional y placement. Vertical placement must be on a Floor or Walkway.
- The editor provides an optional snap-to-grid mode for horizontal placement; vertical floor/walkway alignment is mandatory.
- Definition movement points allow fractional x offsets, but all usable Markers and routing vertices remain at the supporting Floor/Walkway height. Artwork may extend upward; this feature introduces no elevated movement points or slopes.
- This refines the earlier allowance for fractional definition offsets: it applies only to x.
- Moving an instance translates all its usable Markers and routing vertices rigidly by the same placement delta. Vertices are not independently snapped.
- Moving Furniture never teleports Agents at its Markers. Agents retain their physical positions; Markers retain identity at the new positions, with normal Route planning if the behaviour still requires reaching them.
- The Furniture's entire width must be supported by a continuous Floor/Walkway span at one height within its owning Location. Reject placement over a gap or across a Location boundary.
- This supersedes the earlier agreement allowing fractional instance y coordinates; unsupported off-floor placement is not allowed.

## Agreed overlap semantics

- Furniture footprints may overlap on the same Floor/Walkway at different Local depths, including arrangements such as a chair in front of a desk.
- Furniture footprints must not overlap at the same Local depth. Placement, movement, and depth edits must preserve this constraint.
- The footprint is the tile layout's full bounding rectangle, including transparent pixels and gaps between tiles. Boundary contact is allowed and is not overlap.
- Overlapping routes join only at designated external connection points with matching resolved Local depth. A connection may split an existing route at that point.
- Visual crossings or overlaps alone do not create connections. Depth changes require a definition's authored connectivity.

## Agreed deletion semantics

- Reject Furniture deletion if any owned usable Marker is referenced by an Agent behaviour configuration, listing the references, consistent with existing Marker deletion protection.
- Otherwise delete the instance, its owned Markers, and its routes together.
- Refuse Floor/Walkway removal if it would leave any Furniture unsupported; report the blocking instances and require moving or deleting them first.

## Agreed initial examples

- Include a sample catalogue and demonstration World with placeholder chair, two-seat sofa, and desk artwork, exercising simple destinations, multiple Markers, and front/back depth routing.

## Spec handoff

- The full spec is recorded in `docs/tickets/furniture.md`, including user stories, implementation decisions, testing decisions, and scope exclusions.
- The complete-Path secondary depth-continuity objective is recorded in ADR 0017.
- Published the spec as GitHub issue [#347](https://github.com/ajare/promethium-fermide/issues/347) with the `ready-for-agent` label, expanding the original Furniture request #330.
