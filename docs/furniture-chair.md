# Catalogue-backed Furniture (#348–#358)

Furniture supports one-, two-, and larger-tile artwork layouts with individually
authored usable points, explicit isolated front/back routes and Local-depth
placement/editing, matching-depth external attachments and complete-Path depth
continuity, composed floor replacement spans and safe catalogue reconciliation
on World load. There is no sitting state or seat
reservation.

## Complete bundled demonstration (#359)

See [Furniture demonstration and catalogue authoring](furniture-demonstration.md)
for the required `furniture.world.yaml` / `furniture.furniture.yaml` teaching
project, placeholder chair/sofa/desk artwork, front/back observers and portable
place/edit/target/save/reopen workflow. Earlier slice fixtures below remain
compatibility and focused regression examples.

## Authoring

Save a World, put a manually authored `.furniture.yaml` catalogue beside it, then
expand **Furniture** in the World panel and choose **Select Furniture catalogue...**.
The native file picker starts beside the World and filters `.furniture.yaml` files.
Catalogues outside that directory are refused; only the basename is persisted.
Cancellation changes nothing, and load failures appear inline. The header contains
only catalogue selection and its current filename, not Furniture editing controls.
Catalogue selection uses normal document history.

Select a Room, Corridor or Facade and open **Location plan**. While paused, drag a
definition from its palette row into the grid to place an automatically named
instance on the displayed Level and integer Local depth. New placements snap to
whole World-cell X. Placement refusals appear in the preview, and valid releases
use normal document history.

`resources/test-worlds/chair.furniture.yaml` describes the format. Catalogue UUID,
definition key and usable-point key are reference identities, not display labels.
Artwork names an existing `ObjectAtlas` Image-set region; the application resource
manager resolves it and owns its atlas. The geometric placeholder chair is a full
World-tile region in `resources/Resources.yaml`. No Furniture TileSet is created.
The GUI registers user-selected catalogues dynamically as FurnitureCatalogue
resources; headless document tools use the same parser without graphics resources.

## Placement and destinations

The whole artwork bounding rectangle must fit inside one Location and have continuous
Ground or Walkway support. Fractional x, boundary-touching chairs and elevated
Walkways are supported; gaps, overhangs, fractional y, non-finite positions,
same-depth overlap, Backgrounds and Transits are refused before mutation.

Artwork may overlap at different instance Local depths. Coincident owned Markers
remain distinct destinations; visual overlap never connects their private vertices.

Each authored usable point owns an independently named World Marker. Its initial name is
`<instance name> <usable label>`, validated in the existing Marker namespace. It
appears in the ordinary behaviour destination selectors and defaults to **Blocks
pathing**. For definitions without explicit vertices, the graph connects it as a
destination branch off an ordinary floor anchor, so it remains reachable and departable without
severing circulation. Explicit definitions use only their authored edges.
Independent movement or deletion of its owned Marker is refused.

### Side-route safety diagnostics (#370)

A `sideRoutes` footprint replaces ordinary floor routing. Graph builds emit
**error-level** diagnostics when that replacement strands an ordinary row object
(Marker, Door or Window threshold, or physical interaction control), or when the
Furniture network has no ordinary floor attachment. Diagnostics apply regardless
of authoring order, including rebuilds and document loading; they do not refuse
placement or automatically repair the authored World. Move the object or Furniture
out of the replaced span, or leave floor beside an external depth-zero port.
Wall-to-wall replacements have no attachment; a placement against just one wall
can still attach on its other side. A composed network may attach through another
Furniture instance. Furniture-owned destinations retain their explicit branches,
and definitions without `sideRoutes` retain ordinary floor routing.

## Safe instance edits (#349)

Select an existing footprint in **Location plan**. While paused, drag it to change
X or Local depth, holding Shift for fractional X, or use **Delete selected Furniture**.
Edits and deletion use ordinary document undo/redo. Movement stays on the same
supporting Level inside the owning Location and rigidly translates artwork and
every owned point; it cannot cross a Floor/Walkway gap, overhang the Location or
overlap Furniture at the same Local depth. The World panel no longer exposes
instance editing controls.

Instance names generate point names only at placement. Renaming an instance does
not rewrite its Marker name. Rename the owned Marker through ordinary **Marker
Selection**; names use the same World namespace as standalone Markers, and
behaviour references keep the same identity. Markers cannot be independently
moved or deleted. Agents stay at their physical positions when Furniture moves;
existing destination intent follows the same Marker through ordinary Route
planning and Route loss handling, not a teleport or new Furniture movement API.

Deletion reports all Agent behaviour configuration references to the owned
Marker and refuses without mutation or history. Unreferenced deletion removes
both instance and destination, preserving other authored object slots and never
reusing the deleted identities. Walkway removal, Location deletion and resizing
that would crop or invalidate Furniture refuse with blocking instance names.
Delete or relocate the blocking Furniture before removing its support. Local-depth
changes share the same preflight/history actions and preserve owned Marker identities. Negative instance/resolved edge depths and same-depth footprint
overlaps are refused before mutation.

## Location plan selection and movement (#442)

Open **Location plan** from Selection for a Room, Corridor, or Facade. The plan
stays pinned to that Location and displays one supporting Level. Click a labelled
Furniture footprint to select and highlight it; selecting another Location does
not retarget the plan.

While paused, left-drag an existing footprint to preview movement. X snaps to
whole World cells unless Shift is currently held; pressing or releasing Shift
updates the preview immediately. Local depth always uses non-negative integer
rows, and the supporting Level and owning Location do not change. Green previews
are valid; red previews show the existing edit-validation diagnostic. Release
inside the grid to commit one undoable edit. Invalid releases, release outside the
grid, Escape, or right-click leave the original instance unchanged. Movement
preserves owned Marker identities and names. The depth range expands as needed
and does not shrink automatically. Reconstruction, deletion, or catalogue changes
cancel stale gestures and selections.

## Documents

World schema 45 stores a catalogue basename/expected UUID reference, instance
identity/name/placement/Local depth/definition key, a `destinations` array containing
every usable-point key and Marker identity/name/properties, and a non-reused instance
identity allocator. Schema-43 chair and schema-44 multi-point documents remain
readable with default Local depth 0. YAML `.world.yaml` and binary `.world` documents use the same construction records. They resolve the
current catalogue on reopening, not an embedded definition snapshot. Worlds
without Furniture have no catalogue dependency. Move the World and its catalogue
together to preserve portable references; a missing catalogue, missing definition,
malformed dependency, UUID mismatch or invalid current placement is a diagnostic failure.

### Catalogue edits on next load (#357)

The current external catalogue is authoritative; no layout snapshot is saved.
Definition and usable-point keys preserve existing Marker identities, independent
names/properties and behaviour references through label changes and reordering.
New points receive new identities beyond the saved non-reuse allocation mark;
initial names use the current instance name and point label, with a unique
identity-based fallback for conflicting or overlong names. Removed points retire
their identities rather than transferring them to another key. Removing a point
referenced anywhere in an Agent behaviour configuration fails loading with the
instance, definition, point, Marker, Agent and configuration field identified.

Current artwork widths, usable offsets and routes are resolved on load. Complete
footprints must still have Floor/Walkway support within their owning Location and
remain separated from same-depth Furniture; incompatible layouts fail rather
than moving or omitting instances. Both YAML and binary documents use this same
reconciliation. Saving the loaded World persists the reconciled destinations and
advanced allocation mark. Already open Worlds (including Reset and history) keep
their loaded catalogue; there is no live reload, designer, migration UI or
automatic geometry/reference repair.

`resources/test-worlds/chair.world.yaml` demonstrates chairs in all three supported
Location types and an Agent visiting the reading chair. Reaching its Marker uses
ordinary movement, not a new Furniture behaviour API.

## Multi-tile layouts (#350)

`resources/test-worlds/layouts.furniture.yaml` contains a two-seat sofa and a
sparse three-by-two layout. Every `tiles` entry names an `ObjectAtlas` Image-set
region of exactly one World tile (64 × 160 pixels), at integer `x`/`y` offsets.
Offsets may extend horizontally either side of the instance origin; artwork may
extend upward, but never below the supporting Floor. Transparent pixels and
missing tiles inside the bounding rectangle do not shrink its footprint. Entire
horizontal support is checked from the rectangle's left to right edge, including
fractional end cells. Boundary touching is allowed; area overlap is not.

Every `usablePoints` entry has a distinct stable `key`, a distinct initial `label`,
a finite fractional `x` offset within the artwork width, and `y: 0`. All Markers
and graph vertices stay at the instance's supporting Floor/Walkway height, even
for tall artwork. Point order and display labels are not identities. The editor's
snap toggle rounds only the instance x origin, leaving all point offsets rigid.
It never rounds or permits fractional y.

A usable point may specify `blocksPathing: false` to make its Marker usable as
both a destination and an intermediate waypoint. Omitted `blocksPathing` defaults
to `true`. This is a creation default: existing World-owned Marker properties
remain authoritative on save/load, while newly reconciled usable points receive
the catalogue default. Clear Blocks pathing in Marker Selection for existing seats.

```yaml
usablePoints:
  - {key: left, label: Left seat, x: 0.5, blocksPathing: false}
  - {key: right, label: Right seat, x: 1.5, blocksPathing: false}
```

The bundled sofa's front route passes through both non-blocking seat Markers;
the bundled chair uses the same arrangement with one non-blocking seat. Both
have separate back routes, use `sideRoutes: true`, and require instance Local
depth 1 or greater: front edges resolve at instance depth minus 1, back edges at
instance depth plus 1. Approach edges remain at fixed depth 0 to attach to Floor.

Each destination is independently selectable and renameable using the ordinary
Marker/behaviour UI. Renaming the instance does not rename its destinations.
Movement, replay, history, deletion and supported-format round trips handle all
points together; referenced deletion and invalid placement are atomic refusals.
The existing first-destination `marker` convenience member is retained for chair
API compatibility; `destinations` exposes the complete stable-key layout.

## Explicit desk routes (#351)

`resources/test-worlds/desk.furniture.yaml` and `desk.world.yaml` demonstrate an
isolated two-tile desk using existing placeholder Image-set artwork. The catalogue
adds `vertices` and `edges` alongside `usablePoints`:

- Each vertex has a unique `key`, finite `x` offset within the footprint (including
  its right boundary), and optional `y: 0`. A `usablePoint` binding names exactly
  one existing usable-point key at its authored x. Every usable point must be
  bound exactly once. Other vertices are routing-only, never behaviour destinations.
- `external: true` exposes the resolved depths of the vertex's incident authored
  edges for attachment. Only a port with an incident depth-0 edge attaches to the
  surrounding ordinary floor. Undesignated vertices remain private even when
  coordinates coincide.
- Each bidirectional edge names `from` and `to` vertex keys. Missing `depthOffset`
  means fixed Local depth 0; a signed `depthOffset` resolves relative to the
  instance depth. Unknown endpoints, self edges and duplicate connections fail
  catalogue loading. Missing edges are never inferred.
- `sideRoutes: true` declares that the authored routes replace ordinary floor
  edges across the complete artwork width. Definitions without side routes retain
  ordinary floor routing, including definitions with depth-assigned seat branches.
  The desk explicitly joins its fixed-0 left/right approaches to front (`depthOffset: 0`) and back (`depthOffset: 1`) routes. At
  instance depth 2 these resolve to front 2 and back 3, with no ordinary bypass.
- Instance Local depth and every resolved edge depth must be non-negative and
  fit an integer. Invalid placement, movement, depth edits and document replay
  fail atomically. Different-depth artwork overlap is allowed; external route
  attachment is described below.

Local depth is a render-order integer local to a Location, not a Layer or physical
coordinate. Edge geometry, walking duration, permissions and Mobility constraints
are unchanged. Agents adopt their active edge depth at traversal start; depth-changing
waypoints are not skipped. Larger depths render first, and Furniture renders before
Agents at equal depth, under the existing Layer visibility and aperture clips.
Thus a depth-2 walker is in front of the depth-2 desk, while a depth-3 walker is behind it.
Complete-Path depth continuity is described below; it does not change edge route costs.

## Matching-depth attachments (#355)

`resources/test-worlds/attachments.furniture.yaml` adds a non-replacing chair
whose external approach exposes an authored relative-depth edge. A depth-1 chair
at x=3.125 attaches to the middle of a depth-2 desk's front route at x=2.125:
its approach edge has `depthOffset: 1`, resolving to the same depth 2. It does
not attach to the desk's coincident depth-3 back route.

Two designated external points at the same position can connect at their shared
resolved edge depths. An external point strictly inside another instance's route
splits that route and attaches at matching depth. A private endpoint is not an
external port. Splits retain the original horizontal geometry, depth and total
walking distance/duration; their junctions are routing-only, not usable Markers.
Multiple attachments are rebuilt deterministically, independent of placement
order, including coincident ports. No artwork intersection, private vertex
coincidence, edge crossing or mismatched port depth creates a connection.

A port with no incident depth-0 edge cannot acquire a floor connection. A depth
switch requires incident edges explicitly joined by a definition, rather than
an inferred front/back shortcut. Usable Markers retain Blocks pathing semantics,
identity and independent names. Attachments are derived on build, movement,
undo/redo, deletion, Reset and YAML/binary reopening; no additional saved graph
or schema is introduced. Furniture movement leaves Agents' physical positions
and retained depths alone. Two route-replacing instances follow the same explicit attachment rules, as
described below.

## Composed replacement spans (#356)

`resources/test-worlds/composition.furniture.yaml` supplies wide and narrow
replacement layouts with matching resolved depth-2 routes at instance depths 2
and 3. Partially overlapping, nested and boundary-touching spans compose through
designated ports only. A matching port can attach to another definition's route
interior; a private endpoint, mismatched depth or artwork intersection cannot.

Floor suppression considers every replacement span together before adding any
floor attachment. Depth-0 external ports acquire ordinary floor anchors only
where floor remains on at least one side. Boundaries inside another replacement,
and shared boundaries with replacement coverage on both sides, are not floor
junctions. Explicit matching-depth port connections remain possible there;
ordinary Markers at those internal boundaries do not gain inferred connectivity.

Movement and deletion rebuild coverage from the remaining instances, restoring
floor only outside their replacement spans and dropping only removed graph
contributions. Same-depth rectangular overlap is still refused atomically;
different-depth overlap and boundary contact remain valid. No document schema,
extra saved graph, movement timing or editor action is introduced. Construction
replay, repeated builds, undo/redo, Reset and YAML/binary reopening derive the
same authored network.

## Retained Agent depth (#352)

Starting an edge adopts its integer Local depth immediately, with no interpolation,
physical displacement, extra movement duration or route-cost penalty. Arrival,
idling, pause/resume and structural replay retain the incoming depth; a newly
placed Agent has depth 0. Depth-changing waypoints cannot be skipped, including
coincident topology-only connections. Ordinary same-depth skipping is unchanged.
The immutable production `RouteDecisionContext::localDepth` captures departure
context for complete-Path depth-continuity selection.

World schema 46 pairs each saved authored Agent position with its Local depth.
As before, World save/reset restores authored positions and destination intent,
not a runtime simulation checkpoint. Setting a Path or clearing it authors the
current position and its retained depth; explicitly repositioning an Agent starts
new depth-0 history. Saving an underway route restores its authored start and that
start's depth, not a mismatched runtime depth. Older documents default to depth 0.
YAML and binary restoration, Reset simulation and topology replay follow these
same lifecycle conventions.

## Complete-Path depth continuity (#353)

For alternative complete Paths, routing first minimizes total Perceived route cost.
Only an exactly equal primary cost is tied by the total sum of absolute numerical
Local-depth changes, including retained departure depth into the first edge. A
new Agent starts from depth 0; stationary arrival and departure retain history.
Depth is not an epsilon cost penalty, and a locally nearest-depth outgoing edge
need not be the best complete Path.

Search keeps distinct incoming depths at shared vertices and compares complete
Paths lexicographically. Strict improvements and finite arrival states terminate
coincident connectors and equal-cost alternatives deterministically. Graphs with
only depth-0 edges keep the existing reusable primary-cost search; depth-bearing
graphs use lexicographic Dijkstra. Feasibility, two-pass Mobility handling,
Perceived route costs and objective durations are unchanged. Private authored
Furniture edges are never split as ordinary floor by inferred-source seeding:
coincident front/back routes are not interchangeable floor attachments.

## Cross-Sector depth continuity (#354)

Each Sector has an independent Local-depth axis. A boundary traversal contributes
no cross-axis numerical depth change and establishes arrival depth 0; subsequent
edges in that Sector are compared against that fresh baseline. This applies on
every entry, including repeated visits, same-Layer Location boundaries and
adjacent-Layer thresholds. Total primary Perceived route cost still wins before
the secondary depth objective. Runtime retains source context during boundary
crossing and resets it when Sector entry commits; stationary/departure context
then follows the destination's own routes. Local depth does not change Layer,
threshold timing, visibility, Mobility, Access permissions, Route observations,
or Route persistence.

The cross-Sector headless check enumerates complete Paths independently and
compares their primary cost and reset-baseline depth-change score. Room,
Corridor and Facade journeys, three-Layer Door journeys, repeated visits, tiny
primary improvements, inferred departures, runtime entry and stationary context,
access/control refusal, forbidden and last-resort Mobility, remote device state,
valid-Path persistence and repeated-run determinism use production seams.

## Movement through topology edits (#358)

Furniture movement, Local-depth edits and deletion preserve an Agent's physical
position and incoming depth, including stationary Agents and deeper side-route
walkers. A moved usable Marker keeps its identity; an idle Agent does not acquire
new intent merely because that Marker moved. Ongoing destination intent follows
its new position through normal Route planning, with ordinary Route loss if the
destination disappears or no feasible replacement exists.

Pause captures the remaining Path as pointer-free vertex/edge descriptions.
Graph replacement rebinds an unchanged suffix by authored identity, geometry,
edge kind and depth; it never transfers a private coincident front/back vertex
to a different route. Rebound Paths are evaluated at planning expiry using normal
feasibility and Route persistence. Invalid suffixes bypass persistence. Neither
rebuilding nor planning completion changes physical position or retained depth;
the next edge acquires depth only when traversal begins. Continuous walking
resumes without retreating to its source, and zero-length depth boundaries and
Sector-entry depth resets retain their existing semantics. Repeated paused edits
preserve the planning interval and never leave retired graph references in a
planning suffix.

World schema 47 adds stable `destinationMarker` identity to saved authored Paths.
This disambiguates coincident Furniture destinations and follows moved points
without changing the authored-position/depth save/reset contract into a runtime
checkpoint. Structural replay carries that authored origin and intent separately
from the ongoing physical position. Schema 46 and earlier Paths still use their
legacy positional restoration. YAML, binary and editor history use the same
restoration seam. Referenced deletion and supporting-floor removal retain their
existing preflight/no-mutation contracts.

## Headless checks

- `pf-smoke-routing --check furniture/depthContinuity`
- `pf-smoke-routing --check furniture/sectorDepthContinuity`
- `pf-smoke-simulation --check furniture/retainedDepth`
- `pf-smoke-simulation --check furniture/topologyEdits`
- `pf-smoke-world --check furniture/demo`
- `pf-smoke-render --check furniture/demoCommands`
- `pf-smoke-editor --check furniture/demoActions`
- `pf-smoke-world --check furniture/chair`
- `pf-smoke-world --check furniture/layouts`
- `pf-smoke-world --check furniture/deskRoutes`
- `pf-smoke-world --check furniture/attachments`
- `pf-smoke-world --check furniture/composition`
- `pf-smoke-persistence --check furniture/documents`
- `pf-smoke-render --check furniture/chairCommands`
- `pf-smoke-editor --check furniture/chairActions`
- `pf-smoke-editor --check furniture/attachmentActions`
- `pf-smoke-editor --check furniture/compositionActions`

The checks use the existing World/document, CPU draw-command, and production
editor-action seams, isolated temporary roots and deterministic simulation ticks.
They require no OS windows, dialogs or GPU.

## #351 validation

Full incremental default builds and complete CTest inventories passed in Linux
GUI Release (`build-linux`, 85 tests), GUI Debug (`build-debug`, 85 tests),
headless Release (`build-linux-validation/release-headless`, 82 tests), and
headless Debug with `PF_HIGH_ANALYSIS=ON`
(`build-linux-validation/debug-headless`, 82 tests), using `--parallel 4` and
`ctest -j 4`. Displays were unset; the optional vendored GUI capability test was
explicitly skipped in each configuration. Focused Furniture checks and affected
module/CLI contracts passed. `git diff --check` passed. Windows validation is not
claimed.

## #352 validation

Final incremental default builds and complete CTest inventories passed in Linux
GUI Release and Debug (85 tests each), headless Release and high-analysis Debug
(82 tests each), in the same four build trees listed above. Displays were unset,
all checks were non-interactive, and the optional vendored GUI capability test
was explicitly skipped. Focused retained-depth simulation, stationary render,
affected module and CLI-contract checks passed; `git diff --check` passed.
Windows validation is not claimed.

## #353 validation

Final incremental default builds and complete CTest inventories passed in Linux
GUI Release and Debug (85 tests each), headless Release and high-analysis Debug
(82 tests each), in the same four build trees listed above. Displays were unset;
the optional vendored GUI capability test was explicitly skipped. Complete-Path
reference scenarios, repeated-run determinism, inferred stationary departures,
simulation-selected routes after topology replay, affected modules and CLI
contracts passed. The stationary render fixture explicitly uses its depth-2 seat
branch instead of assuming an inferred source approach acquires that depth.
`git diff --check` passed. Windows validation is not claimed.

## #354 validation

Final incremental default builds and complete CTest inventories passed in Linux
GUI Release and Debug (85 tests each), headless Release and high-analysis Debug
(82 tests each), in the same four build trees listed above. Displays were unset;
the optional vendored GUI capability test was explicitly skipped. Focused
cross-Sector scenarios and affected Routing, Simulation, Permissions, Transports
and Render modules/contracts passed. `git diff --check` passed. Windows
validation is not claimed.

## #355 validation

Final incremental default builds and complete CTest inventories passed in Linux
GUI Release and Debug (85 tests each), headless Release and high-analysis Debug
(82 tests each), in the four build trees listed above, using build/test parallelism
4. Displays were unset; the optional vendored GUI capability test was explicitly
skipped. Focused attachment, editor history, YAML/binary replay, physical traversal
and render-command checks passed, together with affected module/CLI contracts.
`git diff --check` passed. Windows validation is not claimed.

## #356 validation

Final incremental default builds and complete CTest inventories passed in Linux
GUI Release and Debug (85 tests each), headless Release and high-analysis Debug
(82 tests each), in the four build trees listed above, using parallelism 4.
Displays were unset; the optional vendored GUI capability test was explicitly
skipped. Focused composition Paths, affected modules and CLI contracts, editor
history, movement/removal permutations, repeated builds, construction replay,
and YAML/binary round trips passed. `git diff --check` passed. Windows validation
is not claimed.

## #358 validation

Final incremental default builds and complete CTest inventories passed in Linux
GUI Release and Debug (85 tests each), headless Release and high-analysis Debug
(82 tests each), using the same four build trees and parallelism 4. Displays were
unset and the optional vendored GUI capability test was explicitly skipped.
Focused topology-edit, coincident attachment/rebinding, planning, Route loss,
authored YAML/binary restoration, editor history and render-command checks passed,
as did affected-module/CLI contracts and the ownership audit. `git diff --check`
passed. Windows validation is not claimed.
