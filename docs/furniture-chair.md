# Catalogue-backed Furniture (#348–#352)

Furniture supports one-, two-, and larger-tile artwork layouts with individually
authored usable points, explicit isolated front/back routes and Local-depth
placement/editing. Complete-Path depth continuity, overlapping-route composition
and catalogue migration remain later tickets. There is no sitting state or seat
reservation.

## Authoring

Save a World, put a manually authored `.furniture.yaml` catalogue beside it, then
expand **Furniture** in the World panel. Enter its basename and load it, select a
definition, select a Room, Corridor or Facade on the canvas, enter the instance
name, Location-local x/supporting Level and non-negative Local depth, and click
**Place Furniture** while paused. The optional snap toggle rounds x only; y must always be floor-aligned.
Refusals appear inline, without dialogs. Catalogue selection and placement use
normal document history.

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

## Safe instance edits (#349)

Select an existing instance in the **Furniture instance** selector in the World
panel. While paused, change its name, Location-local x or supporting Level and
click **Apply Furniture edit**, or click **Delete Furniture**. Edits and deletion
use ordinary document undo/redo. Movement stays inside the owning Location and
rigidly translates artwork and every owned point; it cannot cross a
Floor/Walkway gap, overhang the Location or overlap Furniture at the same Local depth.

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

## Documents

World schema 45 stores a catalogue basename/expected UUID reference, instance
identity/name/placement/Local depth/definition key, a `destinations` array containing
every usable-point key and Marker identity/name/properties, and a non-reused instance
identity allocator. Schema-43 chair and schema-44 multi-point documents remain
readable with default Local depth 0. YAML `.world.yaml` and binary `.world` documents use the same construction records. They resolve the
current catalogue on reopening, not an embedded definition snapshot. Worlds
without Furniture have no catalogue dependency. Move the World and its catalogue
together to preserve portable references; a missing catalogue, missing definition,
removed usable point, UUID mismatch or invalid placement is a diagnostic failure.

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
- `external: true` explicitly designates a fixed-0 attachment to the surrounding
  floor. Undesignated vertices are private even when coordinates coincide.
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
  fail atomically. Different-depth artwork overlap is allowed; this slice does
  not split or compose overlapping private routes.

Local depth is a render-order integer local to a Location, not a Layer or physical
coordinate. Edge geometry, walking duration, permissions and Mobility constraints
are unchanged. Agents adopt their active edge depth at traversal start; depth-changing
waypoints are not skipped. Larger depths render first, and Furniture renders before
Agents at equal depth, under the existing Layer visibility and aperture clips.
Thus a depth-2 walker is in front of the depth-2 desk, while a depth-3 walker is behind it.
No depth-continuity preference has been added to route cost or search.

## Retained Agent depth (#352)

Starting an edge adopts its integer Local depth immediately, with no interpolation,
physical displacement, extra movement duration or route-cost penalty. Arrival,
idling, pause/resume and structural replay retain the incoming depth; a newly
placed Agent has depth 0. Depth-changing waypoints cannot be skipped, including
coincident topology-only connections. Ordinary same-depth skipping is unchanged.
The immutable production `RouteDecisionContext::localDepth` captures departure
context; it does not yet implement a depth-continuity preference.

World schema 46 pairs each saved authored Agent position with its Local depth.
As before, World save/reset restores authored positions and destination intent,
not a runtime simulation checkpoint. Setting a Path or clearing it authors the
current position and its retained depth; explicitly repositioning an Agent starts
new depth-0 history. Saving an underway route restores its authored start and that
start's depth, not a mismatched runtime depth. Older documents default to depth 0.
YAML and binary restoration, Reset simulation and topology replay follow these
same lifecycle conventions.

## Headless checks

- `pf-smoke-simulation --check furniture/retainedDepth`
- `pf-smoke-world --check furniture/chair`
- `pf-smoke-world --check furniture/layouts`
- `pf-smoke-world --check furniture/deskRoutes`
- `pf-smoke-persistence --check furniture/documents`
- `pf-smoke-render --check furniture/chairCommands`
- `pf-smoke-editor --check furniture/chairActions`

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
