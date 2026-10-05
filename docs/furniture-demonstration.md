# Furniture demonstration (#359)

`resources/test-worlds/furniture.world.yaml` is the compact editable chair/desk
sample (`FurnitureDemo`, with `furniture.furniture.lua` beside it).

For the complete integration tour below, open
`resources/test-worlds/furniture-integration.world.yaml`. Keep
`furniture-integration.furniture.lua` beside it; the bundled
`FurnitureIntegrationDemo` resource depends on `FurnitureIntegrationCatalogue`,
which depends on `ObjectAtlas` and its placeholder PNG. Its separate catalogue
keeps the explicit overlapping-route layout independent of the compact sample.
The chair, sofa halves and desk halves are full 64 × 160 World-tile regions in
`resources/Resources.yaml`. No production-quality artwork is intended.

## Tour

On Layer 0, **Showroom** contains a fractional-x reading chair (one Marker), a
Local-depth-2 desk at x=2.125 and a Local-depth-3 two-seat sofa at x=3.125.
Their replacement spans overlap: the sofa's explicitly exposed depth-2 ports
attach to the desk's depth-2 front route, not its coincident depth-3 back route.
The sofa destinations have separate identities and default to Blocks pathing.
A second sofa is supported by the Level-1 Walkway. The Corridor and Layer-1
Facade each contain a chair. The automatic Door at x=10 connects the Showroom
to the Facade; entry resets the independent Sector Local-depth baseline to 0.

Run the simulation to watch **Showroom walker** traverse the composed routes
and reach **Exit**. The two stationary observers share a position within the desk span
but retain depths 2 and 3. The depth-3 observer is behind the desk; the depth-2
observer is in front. The desk is circulation-only and owns no destination
Markers. Select either observer and target **Exit** using the ordinary Marker
destination controls to depart along the explicit side routes. Both may arrive
at the same destination concurrently under Idle. To sit, select a chair or sofa
Marker and explicitly choose **Use furniture**. Occupancy is per seat, not per
instance. A desk has no use scripts; its circulation remains ordinary movement.
The compact sample's Agent explicitly requests Use furniture at its chair. The
complete teaching catalogue also contains a bed whose explicit use sets Lying.

Use the World panel's Furniture header to select the catalogue with its native
file picker. While paused, open the Corridor's Location plan and drag another
chair from the palette into the grid. Hold Shift while moving its footprint for
fractional X, or move it to another Local-depth row. Rename its destination
separately through Marker Selection. Undo/redo uses
normal document history. Save and reopen as either `.world.yaml` or `.world`.
Save/reset restores authored Agent positions, retained depth and Marker intent,
not an underway simulation checkpoint. Furniture movement leaves live Agent
positions/depth alone and uses normal Route planning for invalid Paths.

## Catalogue authoring recipe

The teaching catalogue is a native Lua module returning immutable definition
objects, not a definition designer. Each chair/sofa owns Sitting/claim use and
Standing/release finish functions; bed owns Lying/claim and the same finish.
Stateless helper functions are shared within a catalogue, but occupancy and
instance state are host-managed. Desk has neither callback.
See [the complete authoring reference](furniture-chair.md) for schema and editing
rules. In particular:

- Keep the catalogue UUID, definition `key` and usable-point `key` stable. Labels,
  definition order and point order are not identities. Worlds store a relative
  catalogue basename and expected UUID, not a copy of the definition layout.
- Each `tiles` entry names an `ObjectAtlas` region at integer x/y tile offsets.
  Artwork extends upward from its Floor; the complete rectangular footprint,
  including transparent pixels/gaps, requires continuous support in one Location.
- `usablePoints` and `vertices` use finite, possibly fractional horizontal offsets.
  Their vertical offset is always zero: points are floor-aligned, never elevated
  by artwork. Usable points bind once to vertices in explicit layouts; other
  vertices remain routing-only and do not appear as destinations.
- An edge with no `depthOffset` stays fixed at 0. An assigned offset is relative
  to the instance depth: the desk's 0/1 offsets resolve to front 2/back 3. The
  sofa's -1 resolves to 2 at instance depth 3; placing that sofa at depth 0 is
  invalid. Depth is ordering, not distance, speed or a primary route-cost penalty.
- Only `external = true` ports expose incident edge depths. Matching-depth ports
  can join or split another instance's route; geometric overlap alone cannot.
  Only incident fixed/resolved depth-0 ports connect to surviving ordinary floor.
  Author all internal/depth-changing edges explicitly. `sideRoutes = true`
  replaces the full floor span; overlapping spans cannot restore a floor bypass.
- Usable points default to Blocks pathing, preventing them from being intermediate
  waypoints while allowing arrival and departure. The integration sofa seats are
  destination branches; its exposed depth-2 ports provide through-circulation.

## Safe edits and diagnostics

Edit catalogues while their Worlds are closed, then reopen. Stable keys retain
Marker IDs, independent names/properties and behaviour references. Changed point
positions and routes take effect on the next load; new points receive fresh IDs.
Unreferenced removed points retire their IDs. Removing a behaviour-referenced
point fails with instance, definition, point, Marker, Agent and field diagnostics.
Paused **Reload Furniture catalogue** validates transactionally before replacing
the package and finishes affected active uses with their old callbacks. There is
no migration UI, inferred connectivity or automatic repair.

Missing catalogue/definition/artwork, wrong catalogue UUID, malformed definitions,
negative resolved edge depth, unsupported width, boundary overhang or same-depth
footprint overlap are incompatibilities, not permission to omit Furniture or
substitute routes. Restore the dependency or deliberately correct the catalogue
and authored placement/reference. Supporting Floor/Walkway removal and referenced
Furniture deletion are refused before mutation. Different-depth overlap does not
relax Location permissions, Mobility constraints, Layer visibility or aperture
clipping. Legacy Furniture-free Worlds require no catalogue.

## Headless integration

#465 converted all seven bundled catalogues, all five dependent Worlds and the
resource manifest. An explicit audit found no conflicting legacy per-point uses:
the compact catalogue's chair/sofa points uniformly used Sit and bed used Lying;
other catalogues omitted uses. Those chair/sofa definitions now deliberately offer
Sitting/claim. The `larger`, `outer` and `inner` layout examples retain Idle-only
intent; no use is invented for them or any desk. Geometry, keys, UUIDs, artwork,
explicit routes and placement were not repaired or changed.

Editable teaching data stays in `resources/test-worlds`; the separate
`furniture-integration` World/catalogue remains stable integration content.
Unconverted regression inputs are frozen under `src/headless/smoke/fixtures/legacy-furniture`;
fixture builders and final loader removal are outside #465.

Required examples fail if absent; no check skips them. Existing module ownership
and unique temporary roots are retained:

- World `furniture/bundledLua`: every converted definition's geometry/identity audit,
  headless loading, circulation, Idle/use/finish, independent sofa seats, bed Pose,
  all five dependent Worlds and YAML/binary round trips.
- Editor `furniture/bundledLuaWorkflow`: every converted catalogue and definition's
  selection, placement, deletion and both-format document reopening, CPU-only.
- World `furniture/demo`: multi-destination/Walkway demonstration, stationary
  departure and equal-cost retained-depth route choices through public APIs.
- Persistence `furniture/documents`: bundled overlapping routes, live paused edit,
  structural replay, stable target intent, relocation, edited-catalogue resolution,
  both formats, simulation Reset and a subsequent Facade journey/Sector reset in
  one workflow, alongside legacy and incompatibility regressions.
- Render `furniture/demoCommands`: manifest dependencies, required tile regions,
  overlapping sample artwork, retained front/behind Agents and Layer/aperture
  command clipping, without a GPU or image-snapshot comparison.
- Editor `furniture/demoActions`: compact-sample production placement/edit/history
  actions, independent Marker rename/target and both-format save/reopen/arrival.

The World, Persistence and Render integration checks use
`furniture-integration.world.yaml` and its dedicated catalogue. Keep their rich
layout intact rather than reducing assertions when the compact sample changes.

Existing Routing, Simulation, World, Persistence, Render and Editor coverage
continues to cover permissions, Mobility, Facades, topology edits and legacy
formats. These tests use bounded simulation ticks, not wall-clock correctness.
No Furniture-specific device, behaviour API or Shared resource is introduced.

## #465 final Linux validation

Final source state built the complete default core/headless/editor inventory in
Release and Debug with `--lane final --build-only all`. Unfiltered final CTest in
each configuration: **103 passed, one optional GUI capability skipped, six failed**.
The six failures are the existing legacy implicit-use migration checks deferred
to #466/#467: Simulation (`actions`, `bedLyingLifecycle`, `authoredChairArrival`,
`seatedEdits`, `occupancyLifecycle`), Routing (`seatRouting`) and Render
(`bedRenderOffset`), counted across module/contract CTests. Their frozen inputs
and assertions are unchanged; no new failure remains. World, Persistence, Editor
and their CLI/exhaustive contracts passed. The new bundled World/editor checks
also passed directly in both configurations. No displays or dialogs were used;
`git diff --check` passed. No Windows validation is claimed.

Retained build evidence: Release `21c9750d14ce45bea9708ae47b257776`, Debug
`14a8c0792a4a4dcbaf5b4a1c2e87c808`. Unfiltered CTest evidence: Release
`cafeafc652034cdd9af98caba5336a1a` (110.16 s), Debug
`f461ad2021584a72b95a6e26a73a271f` (492.46 s).
Full-suite green remains the final integrate-and-verify ticket's contract.

## Historical #359 Linux validation

Incremental default builds and unfiltered CTest inventories passed with
`--parallel 4` / `ctest -j4` in GUI Release (`build-linux`, 85 tests), GUI Debug
(`build-debug`, 85 tests), headless Release
(`build-linux-validation/release-headless`, 82 tests), and headless Debug with
`PF_HIGH_ANALYSIS=ON` (`build-linux-validation/debug-headless`, 82 tests).
Displays were unset; the optional vendored display-dependent capability check
was explicitly skipped in each inventory. Focused demonstration workflows,
affected modules/CLI contracts, ownership audit, decoded atlas/metadata checks
and `git diff --check` passed. High-analysis warnings remain non-fatal; Windows
validation is not claimed.
