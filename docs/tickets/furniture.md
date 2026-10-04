## Problem Statement

World authors cannot currently place reusable furniture in Rooms, Corridors, or Facades. They need chairs, sofas, desks, and similar objects that use World-tile-sized artwork, expose meaningful Agent destinations, and distinguish movement in front of and behind an object without introducing additional Layers.

Existing ordinary floor routing and Agent rendering do not express this sector-local ordering. Adding artwork alone would leave Agents walking through furniture, while treating depth as distance or a normal route-cost penalty would change movement timing and could favour a worse Path. Authors also need reusable definitions, stable destination identities, safe editing, and portable World documents.

## Solution

Introduce Furniture definitions in separate, manually authored Furniture catalogues. A definition describes its tile artwork, usable points, routing vertices, explicit edges, and external connection points. Authors select definitions in the editor and place Furniture instances in any Room, Corridor, or Facade.

Each instance has a name, horizontal position, supporting Floor/Walkway height, and non-negative Local depth. Horizontal placement may be fractional, with optional editor grid snapping; vertical placement is always floor-aligned. Instances may overlap at different depths, but their rectangular footprints cannot overlap at the same depth.

Usable furniture points are individually selectable, stable-identity Markers. Explicit furniture routes replace ordinary floor routing over their span where front/back routes are supplied. Agents render according to their active edge's depth and prefer depth-continuous Paths only when total Perceived route cost is equal. This feature adds destinations and route/render ordering, not sitting, seat occupancy, or reservations.

## User Stories

1. As a World author, I want to place Furniture in a Room, so that I can furnish occupied indoor spaces.
2. As a World author, I want to place Furniture in a Corridor, so that circulation spaces can contain useful objects.
3. As a World author, I want to place Furniture in a Facade, so that open-perimeter Locations retain the same furniture capabilities as Rooms.
4. As a catalogue author, I want to define reusable Furniture separately from a World, so that several Worlds can share the same objects.
5. As a catalogue author, I want to author catalogue files manually, so that I can create definitions without a visual definition designer.
6. As a catalogue author, I want a definition to reference Image-set regions, so that Furniture uses the established application asset system.
7. As a catalogue author, I want each artwork tile to match a World tile's size, so that Furniture fits the World's visual scale without arbitrary scaling.
8. As a catalogue author, I want to compose multiple tiles at integer offsets, so that a sofa or larger object does not require a special rendering type.
9. As a catalogue author, I want stable definition keys separate from display names, so that renaming or reordering definitions does not retarget existing instances.
10. As a catalogue author, I want to specify each usable point individually, so that a chair can have one destination and a sofa can have several.
11. As a catalogue author, I want stable usable-point keys separate from labels, so that existing destination identities survive catalogue label changes.
12. As a catalogue author, I want fractional horizontal point offsets, so that destinations and approaches align accurately with the artwork.
13. As a catalogue author, I want to define routing-only side waypoints, so that Agents can pass on either side of Furniture without treating those waypoints as destinations.
14. As a catalogue author, I want to author internal edges explicitly, so that the engine does not invent unwanted seat or side-route connections.
15. As a catalogue author, I want to designate external connection points, so that I control how Furniture joins the surrounding movement network.
16. As a catalogue author, I want unassigned edge depth to remain fixed at 0, so that ordinary approaches retain front-most routing semantics.
17. As a catalogue author, I want explicit edge-depth offsets relative to an instance's depth, so that one desk definition can produce front and back routes wherever it is placed.
18. As a catalogue author, I want to author the connections between depth-0 approaches and deeper routes, so that placing Furniture does not invent depth-changing shortcuts.
19. As a World author, I want to select a Furniture definition from its catalogue, so that I can place instances without duplicating their layouts.
20. As a World author, I want fractional horizontal placement, so that Furniture is not limited to integer cell positions.
21. As a World author, I want optional horizontal grid snapping, so that I can choose precise alignment when useful.
22. As a World author, I want mandatory Floor/Walkway alignment vertically, so that Furniture is never placed at an unsupported fractional height.
23. As a World author, I want validation of the entire Furniture width, so that an instance cannot overhang a gap or straddle a Location boundary.
24. As a World author, I want to place Furniture on Walkways as well as ground-level Floors, so that elevated occupied spaces can be furnished.
25. As a World author, I want to move an instance as one rigid layout, so that its artwork, Markers, and routing vertices remain aligned.
26. As a World author, I want an editable instance name, so that I can identify a particular piece of Furniture.
27. As a World author, I want usable Markers to receive initial names derived from the instance and point labels, so that a sofa's seats are understandable in destination selectors.
28. As a World author, I want to rename usable Markers independently, so that I can describe their purpose without changing identity.
29. As a behaviour author, I want to select furniture-owned Markers through the existing destination mechanism, so that Furniture does not require a competing behaviour destination type.
30. As a behaviour author, I want furniture destinations to keep their identities after movement, rename, and save/load, so that existing Agent configurations remain valid.
31. As a World author, I want usable Furniture Markers to default to Blocks pathing, so that passing Agents do not route through chair or sofa destinations.
32. As a simulation user, I want blocked-through-pathing Markers to remain valid origins and destinations, so that Agents can still reach and leave Furniture.
33. As a World author, I want ordinary floor connections replaced where front/back furniture routes exist, so that Agents cannot use a depth-0 shortcut through the same span.
34. As a World author, I want Furniture without side routes to leave ordinary floor routing intact, so that a simple destination does not unintentionally sever circulation.
35. As a World author, I want Furniture footprints to overlap at different depths, so that I can place a chair in front of a desk.
36. As a World author, I want same-depth footprint overlap rejected, so that render order is not ambiguous between intersecting pieces.
37. As a World author, I want overlap checks to use complete tile-layout bounding rectangles, so that transparent pixels and layout gaps do not produce surprising placement rules.
38. As a World author, I want boundary-touching footprints accepted, so that adjacent pieces can be placed directly beside one another.
39. As a catalogue author, I want overlapping routes to join only at matching-depth external connection points, so that visual overlap alone does not create shortcuts.
40. As a simulation user, I want larger Local depths rendered farther back, so that Agents and Furniture have predictable sector-local ordering.
41. As a simulation user, I want an Agent at a Furniture instance's depth rendered in front of that instance, so that the authored front-route convention works.
42. As a simulation user, I want an Agent on a deeper route rendered behind Furniture, so that back routes are visibly distinct.
43. As a simulation user, I want an Agent to adopt its active edge's depth immediately, so that render order follows the chosen Path without depth animation or additional movement time.
44. As a simulation user, I want routing to minimise total Perceived route cost before considering depth, so that depth preference never selects a more costly Path.
45. As a simulation user, I want equally costly complete Paths compared by total numerical depth change, so that Agents prefer continuity with their current route.
46. As a simulation user, I want depth continuity reset when entering another Sector, so that unrelated local depth numbers are not compared.
47. As a simulation user, I want stationary Agents to retain their incoming depth, so that stopping at a destination does not cause a visual pop or lose departure continuity.
48. As a simulation user, I want Agents without edge history to start at depth 0, so that their initial rendering and routing baseline is defined.
49. As a simulation user, I want depth-changing waypoints to remain non-skippable, so that movement optimisations do not erase render-order changes.
50. As a World author, I want moving Furniture to leave Agents at their physical positions, so that editing never teleports people along with an object.
51. As a simulation user, I want invalidated Paths to use normal Route planning after furniture edits, so that topology changes follow established movement behaviour.
52. As a World author, I want Furniture deletion refused when its Markers are referenced by behaviour configurations, so that I do not silently break destinations.
53. As a World author, I want unreferenced Furniture deleted together with its owned Markers and routes, so that no orphaned movement objects remain.
54. As a World author, I want supporting floor removal refused while Furniture depends on it, so that a structural edit cannot leave unsupported objects.
55. As a World author, I want catalogue references to remain portable when moving a project, so that its Worlds do not depend on machine-specific absolute paths.
56. As a World author, I want expected catalogue identity verified, so that replacing a catalogue file cannot silently reinterpret Furniture definition keys.
57. As a World author, I want clear loading failures for missing or incompatible Furniture dependencies, so that a World never silently opens with different routing.
58. As a catalogue author, I want Worlds to use revised definitions on their next load, so that shared layouts do not drift into per-World snapshots.
59. As a World author, I want invalid placements or removed referenced points diagnosed after catalogue edits, so that I can repair incompatibilities instead of losing behaviour references silently.
60. As a World author, I want Furniture instances and Marker identities preserved in supported World document formats, so that reopening a World retains its authored layout and destinations.
61. As a World author, I want existing Worlds without Furniture to continue loading and routing normally, so that adopting the feature does not force catalogue dependencies onto old documents.
62. As a World author, I want a sample chair, two-seat sofa, desk, and demonstration World, so that I can learn how simple destinations, multiple points, and front/back routes are authored.

## Implementation Decisions

### Domain boundaries and ownership

- Furniture is a placed object in a Location: Room, Corridor, or Facade. It is not hosted by a Background or Transit. Preserve the Facade's existing object-hosting and traversal invariants.
- Separate reusable Furniture definitions, Furniture catalogues, and World-owned Furniture instances. Definitions describe appearance and movement layout; instances hold authored placement, Local depth, name, definition reference, and distinct usable-point Marker identities.
- Furniture-owned Markers remain World-owned destinations in the established Marker namespace. Side waypoints are routing-only vertices. Do not introduce arbitrary-vertex behaviour destinations or a second destination registry.
- Instances do not override their definition's layout initially. Owned movement points cannot be moved independently in ways that would become per-instance layout overrides.
- Reaching a usable point is ordinary Marker arrival. Multiple Agents may target the same point; this feature introduces no occupancy authority, sitting state, capacity, or reservations.

### Catalogues, assets, and documents

- Introduce a separately persisted, manually authored Furniture catalogue through the existing application resource system. Use established catalogue/document and Image-set conventions rather than introducing a parallel TileSet authority.
- Artwork consists of World-tile-sized Image-set regions at integer tile offsets. One-, two-, and larger-tile appearances use the same representation; tiles are not arbitrarily scaled.
- Definitions and usable points have stable keys separate from display names. Catalogue documents have stable UUID identity. Definition labels and entry ordering are not reference identities.
- World references store a portable relative catalogue filename and expected UUID, following existing external-registry identity protection. A catalogue identity mismatch is a load failure even if keys happen to match.
- Extend the common World schema and construction/replay contracts for instances, catalogue dependencies, and owned Marker identities. Both supported World document representations must carry equivalent semantics. Preserve existing schema/record compatibility conventions and loadability of Worlds without Furniture.
- Loading resolves current catalogue definitions, not saved per-World layout snapshots. Reconcile usable points by stable key, preserving existing Marker identities and names. Newly introduced usable points receive new identities under established Marker allocation rules; removed referenced usable points cause load failure.
- Missing catalogues, missing definitions, mismatched catalogue identity, invalidated placements, and removed referenced usable points produce clear diagnostic load failures. Do not omit Furniture or substitute routing placeholders.
- Catalogue changes take effect on the next World load. Live catalogue reload and its migration UI are not included.
- Generate initial usable-Marker names from the instance name and usable-point label, such as an instance name followed by its seat label. Preserve existing Marker naming validation and independently editable names; behaviour references use stable identities.

### Placement, overlap, and authoring

- Allow fractional instance x coordinates. Instance y must align to a supporting Floor or Walkway; no fractional vertical placement is allowed.
- Require continuous support under the entire artwork width at one height inside one owning Location. Reject placement over a floor gap or across a Location boundary.
- All movement points remain at the supporting floor height, with fractional x offsets allowed. Artwork may extend upward, but Furniture creates no elevated movement points or slopes.
- Define the footprint as the complete artwork tile-layout bounding rectangle, including transparent pixels and gaps. Same-depth footprints may touch at their boundaries but must not overlap in area. Different-depth overlaps are allowed.
- Apply placement, support, non-negative-depth, and overlap validation consistently to creation, movement, depth edits, document replay, and catalogue reconciliation. Refused edits leave the authored World unchanged and provide a useful reason.
- Editor instance workflows cover catalogue selection, placement, selection, movement, naming, Local-depth editing, deletion, and optional horizontal grid snapping. Vertical floor alignment is mandatory regardless of the snap toggle.
- Translation moves artwork, owned Markers, and routing vertices rigidly. Moving Furniture never teleports Agents; Markers retain identity at their new positions.
- Reject Furniture deletion if an owned usable Marker is referenced by an Agent behaviour configuration, listing the references. Otherwise remove the instance, its owned Markers, and its routes together.
- Reject Floor/Walkway removal when it would leave Furniture unsupported; identify the blocking instances rather than leaving them floating or silently deleting destinations.
- Integrate instance authoring with the existing document-edit/history mechanisms rather than establishing a separate mutation authority.

### Graph construction and route composition

- Definitions explicitly describe vertices, internal edges, and designated external connection points. Position coincidence alone does not imply internal connectivity.
- Usable Furniture Markers default to Blocks pathing. They remain valid origins/destinations, but not intermediate waypoints. Do not sever the ordinary floor chain merely by inserting a blocking seat Marker at the same physical position.
- Where definitions supply front/back routes, those routes replace the ordinary floor connection across the Furniture width. No ordinary depth-0 bypass remains through that replaced span. Furniture without side routes leaves ordinary floor routing unchanged.
- Overlapping routes connect only through designated external connection points at matching resolved Local depth; split an existing route at an eligible point when necessary. Visual crossings or overlaps alone create no connection. Depth changes require authored connectivity.
- Local depth is a non-negative integer local to a Sector, not another World Layer and not physical distance. Increasing values are farther back; 0 is front-most.
- Ordinary edges and definition edges without assigned depth remain fixed at 0. Explicit depth offsets resolve relative to the instance depth. Reject placement or edits producing a negative resolved depth.
- Definitions explicitly connect fixed-depth-0 approaches to their relative-depth routes. Placement must not manufacture unspecified depth-changing connections.
- Preserve existing hard traversal feasibility, Mobility-use passes, Location permission requirements, device-operation permission rules, and Route observations. Furniture depth does not override any of these rules.

### Path selection and Agent movement

- Compare complete Paths lexicographically: first minimise total Perceived route cost; only among equally costly Paths minimise total absolute numerical Local-depth change. Do not add an epsilon depth penalty to the primary cost or use an outgoing-edge-only greedy rule.
- Include the transition from the Agent's current/retained depth to the first edge. Within a Sector, numerical gaps matter; depth is not merely an ordinal rank.
- Entering another Sector resets the destination Sector's continuity baseline to 0; do not compare the two Sectors' unrelated numbers.
- Route search must retain enough arrival-depth context to evaluate this history-dependent secondary objective correctly. Paths reaching the same vertex with different incoming depths cannot be indiscriminately collapsed if that would lose the optimal continuation. The search data structure remains an implementation choice, not a test contract.
- Agents adopt an active edge's integer depth immediately on starting that edge. A stationary Agent retains its incoming edge's depth; an Agent without edge history starts at 0.
- Local-depth changes add no physical distance, movement duration, animation interval, or primary Perceived route-cost penalty. Normal edge geometry and walking rules continue to govern movement.
- A waypoint where a Path changes Local depth is non-skippable, even without interaction. Preserve depth boundaries when handling coincident topology-only vertices.
- Furniture edits that rebuild topology retain Agent physical positions and current depth. Invalidated Paths use existing Route planning and Route loss behaviour; the replacement edge supplies depth when traversal starts.
- A moved destination remains the same Marker identity. Agents move to its new position through normal routing only if their current behaviour still requires that destination.

### Rendering and sample content

- Extend the existing backend-independent World draw-command pipeline, consumed by the established renderer. Do not create a second rendering or texture/resource ownership system.
- Render Local depths back-to-front. Within a depth, render Furniture before Agents. Thus a depth-2 Agent is in front of a depth-2 desk and a depth-3 Agent is behind it.
- Retain existing selected-Layer visibility, aperture clipping, recursive Layer rendering, and wireframe semantics. Local depth does not expose other Layers or change the World spatial structure.
- Include a sample catalogue and demonstration World with placeholder chair, two-seat sofa, and desk artwork. Demonstrate simple destinations, distinct sofa Marker identities, front/back routing, different-depth overlap, and the route/render depth conventions.

## Testing Decisions

- The user confirmed three existing testing seams: World/document headless integration, the World draw-command/render boundary, and editor action surfaces. Prefer the World/document seam for domain outcomes; use render and editor seams only for behaviour not observable there. No new low-level test-only seam is required.
- A good test constructs or loads a small World through production APIs, performs an authored action or simulation step, and asserts externally meaningful results: acceptance/refusal, identities, destination resolution, selected Paths, physical positions, render commands, or diagnostic failures. Do not assert private storage, allocator internals, search expansion order, or the exact search algorithm.
- Keep checks in the existing World, Routing/Simulation, Persistence, Render, and Editor smoke ownership boundaries. Reuse the current harness, fixtures, isolated temporary roots, deterministic simulation ticks, and direct module/CTest registration conventions. Do not create a cross-domain test executable or duplicate checks in the legacy compatibility runner.
- Prior art includes World Marker identity and Blocks pathing checks; routing perceived-cost/reference-oracle and planning/topology scenarios; external Agent tag registry identity/portability checks; common World document round trips; Facade and Layer draw-order checks; and editor document-history, palette, and property-action checks.
- Placement tests cover Room/Corridor/Facade acceptance, unsupported Sector kinds, fractional x with integer floor-aligned y, Walkway placement, complete-width support, gaps, Location boundaries, and rigid translation of multiple points. Confirm snap-disabled and snap-enabled editor results without allowing fractional y.
- Footprint tests cover same-depth overlap refusal, different-depth overlap acceptance, depth edits that would create overlap, touching boundaries, transparent artwork, and gaps inside the tile-layout rectangle. Verify refused actions do not partially mutate Furniture or owned Markers.
- Marker tests cover multiple usable points, stable IDs through rename/move/replay/round trip, initial naming and independent rename, existing naming validation, Blocks pathing defaults, valid origins/destinations, and routing-only side waypoints absent from destination selectors.
- Graph outcome tests prove that a simple destination leaves floor circulation intact; explicit front/back routes remove the ordinary shortcut; seats do not become through-waypoints; and author-defined connectivity, not visual coincidence, controls accessibility. Exercise overlapping instances, matching-depth external attachment and route splitting, unmatched depths, and crossings that must not connect.
- Depth-resolution tests cover fixed-0 edges, relative front/back offsets, negative-result refusal, and placing the same definition at several instance depths.
- Route-choice tests include a cheaper complete Path with worse depth continuity; equal primary-cost Paths with different accumulated depth change; the numerical example of incoming depth 3 versus candidate depths 0 and 5; stationary departure; an Agent without history; and independent Sector depth baselines. Include a case with equal-primary-cost arrivals at one vertex but different incoming depths, where retaining arrival context is necessary for the globally correct result.
- Exercise zero-length/coincident authored connections and repeated equal-cost alternatives to detect accidental depth-skipping or cycling without asserting implementation internals. Retain existing deterministic replay and route-cost regressions; do not introduce wall-clock timing thresholds as correctness criteria.
- Simulation tests verify immediate edge-depth adoption, retained depth on stopping, non-skippable depth boundaries, unchanged physical movement timing, ordinary Marker arrival without exclusive occupancy, no Agent teleport on Furniture movement, and normal planning/loss after topology invalidation. Confirm existing permission and Mobility constraints still govern Furniture destinations and routes.
- Deletion tests cover unreferenced atomic instance/Marker/route removal, reference-protected refusal with useful diagnostics, and refusal to remove supporting Floor/Walkway geometry.
- Catalogue/document tests use temporary authored catalogues and supported World formats. Cover portable references, expected UUID mismatch, missing catalogue/definition, current-definition resolution on reload, identity preservation under label changes/reordering, removed referenced points, invalidated geometry, and legacy Worlds without Furniture.
- Render-command tests cover one/two/larger tile layouts, unscaled World-tile size, fractional x placement, larger-depth-first ordering, Furniture-before-Agent at equal depth, stationary Agent depth, same-Layer overlaps at different depths, and existing aperture/Layer clipping. Prefer command semantics to image snapshots or GPU-specific assertions.
- Editor tests exercise real production authoring actions and scoped CPU-only UI state for selection, placement, snap toggling, movement, names, depth edits, refusal diagnostics, deletion protection, and document history. Core domain tests must not acquire editor/GPU dependencies.
- The sample catalogue and demonstration World are required fixtures. Missing required fixtures fail rather than skip. Run relevant existing module and contract checks alongside the new coverage; retain headless operation without windows, dialogs, or required user input.

## Out of Scope

- Sitting poses, seat-use state machines, exclusive occupancy, capacity management, reservations, queues, or new Shared resources.
- New Furniture-specific interactions, device commands, or behaviour scripting APIs beyond existing Marker destinations.
- A physical third dimension, depth interpolation, depth-dependent speed, extra movement time, or depth as a primary route-cost preference.
- Fractional vertical placement, floating Furniture, elevated usable points, or Furniture-created slopes.
- Same-depth footprint overlap, unsupported floor overhangs, or placement across Location boundaries.
- Furniture in Backgrounds or Transits.
- Per-instance layout overrides, arbitrary artwork scaling, and automatically inferred internal or cross-depth connections.
- A visual Furniture-definition designer, live catalogue reload, or automatic repair of incompatible catalogue changes.
- Saved per-World copies of definition layouts, silent dependency omission, and routing placeholders for missing assets/definitions.
- Production-quality furniture art beyond the required placeholder examples.

## Further Notes

- The interview decisions are agreed, and the user explicitly approved the testing seams. This spec authorises no unrequested feature implementation by the spec-writing step itself.
- Follow the existing ADRs for ordered Layers and aperture rendering, Facade invariants, World ownership, external registry identity, resource-managed Image sets and backend-independent drawing, optimal Perceived route cost, Mobility constraints, and independent Location/control permission requirements.
- The non-obvious complete-Path depth-continuity decision is recorded in ADR 0017. The route search must remain correct for its secondary objective without weakening the primary objective established by ADR 0014.
- Exact type names, catalogue suffix/schema numbering, UI arrangement, and search-state representation are implementation details. Choose them consistently with existing conventions; the behaviours above are the acceptance contract.
- This spec expands the original Furniture feature request #330.
- Issue triage label: ready-for-agent.
