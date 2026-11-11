# Sector textures

There is one sector renderer. `pf-render` loads the definition, installs the atlas
as a Willpower `ImageSet` resource, and repeats its regions into the World command
stream; the editor and the shipped game both draw from that same code and see the
same result. The editor keeps no sector rendering of its own, and neither does the
game. See [ADR 0010](adr/0010-render-world-canvases-with-mpp-and-manage-assets-as-resources.md)
and the `pf-render` module header for the boundary this sits behind.

The renderer draws a Sector's surface one tile per World cell: the drawn area is
cut into the cells it covers, each cell is resolved to a tile, and each is drawn at
the World cell's screen size, cropped rather than stretched where the cell is
partial. The grid is anchored on the bottom of the Sector's area - its
floor line - so the tile's own bottom edge lands on the floor and further rows
stack upward. A Sector shorter than a full cell, a 0.7 Corridor or a 0.9 Room,
therefore keeps the end of the tile it was drawn beside at one artwork pixel to
one screen pixel, and drops the top of the tile that would have stood above its
ceiling. Anchored on the top instead, every short Sector would lose its floor and
keep its ceiling band down at floor level. Room interiors therefore support
arbitrary height without intermediate floors. Backgrounds and Facades intentionally retain their authored
opaque colours and take no tile at all - they are authored flat colour by domain
definition, and the renderer fills them without asking the atlas for a surface.
Vehicles, stair and ladder mechanisms, thresholds, other objects, Agents and editor
overlays retain their existing rendering. Aperture clipping and render order are
unchanged; wireframe mode remains primitive-based.

The whole of that rule is pinned before the draw path changes. `smoke/render/SectorSurfaceDrawContract.cpp`
(#1037) records the commands a no-variant Sector surface emits and asserts the
tile region, destination rectangle and tint of every cell, the repeat count, the
floor-line anchor, and the crop a partial top level takes. It is a contract, not a
snapshot: the per-cell draw loop that Sector surface variants need (#1036) has to
satisfy what that file states, so a decorative feature cannot quietly re-anchor
every short Room and 0.7 Corridor in every saved World.

That loop asks a resolver for each cell's tile instead of being handed one name for
the whole area (#1039). Every cell answers the Sector type's own base tile today -
nothing records a per-cell choice yet, so nothing draws differently - and the
resolver is where a per-cell answer appears later without the loop being touched
again, because the loop is the part holding the anchor, the crop and the culling a
variant must not disturb. `smoke/render/SectorSurfacePerCellResolution.cpp` pins
that seam: every cell of the grid resolved exactly once in the Sector-local frame -
column counted from the drawn area's left edge, level from its floor line - each
cell drawing the tile it resolved to on the rectangle its own World cell occupies,
a different tile changing the picture and never the geometry, and a cell the clip
hides entirely never resolved at all. `drawSectorTileSurface(kind, ...)` remains as
the same loop with one name answered for every cell.

## SectorAtlasNew

`resources/textures/sectors-new.tileset.yaml` is the version 2 authored tileset
behind `SectorAtlasNew`: ten full-cell Infrastructure tiles on the 128x320 grid
at 1280x1280, one for each of the ten surface-bearing Sector types - `room`,
`corridor`, `ladder`, `lift`, `shuttle`, `stairwell`, `staircase`, `airlock`,
`chamber` and `dumbwaiter`. Every tile is drawn at one full World cell with no
render offset, because the repeat is per World cell; the loader refuses a surface
drawn at any other size or offset off its cell's anchor.

The ten base tiles sit in a single row across the top of the sheet - columns 0 to
9 of row 0 - rather than in the 5x2 block they filled at 640x640. They were moved
there to make room for what now hangs beneath them.

### Variant families

Each base sprite declares how many variants its type has:

```yaml
  corridor:
    cell: [1, 0]
    variants: 3
    content: [0, 0, 128, 320]
    tintable: true
    offset: [0.0, 0.0]
    drawn_size: [1.0, 1.0]
```

That one number is the whole of what is authored about a family. The loader
expands it into `-V1` .. `-VN`, each in the base's own column one row below the
tile above it, and each inheriting the base's content rectangle, render offset,
drawn size and tintable flag:

| Tile | Cell | Region |
| --- | --- | --- |
| `corridor` | `[1, 0]` | `x 128, y 0` |
| `corridor-V1` | `[1, 1]` | `x 128, y 320` |
| `corridor-V2` | `[1, 2]` | `x 128, y 640` |
| `corridor-V3` | `[1, 3]` | `x 128, y 960` |

The name and the cell both come from the rule rather than from the file, so they
cannot be written into disagreement with each other, and a variant cannot be given
art that does not line up with the base it is a variant of. `-V` is used rather
than a role suffix because it is the one prefix clear of the `-L0`, `-M0` and
`-XY` role vocabulary the object atlases author with.

What the format refuses, at load:

- **A family that runs past the bottom of the declared sheet.** `-Vn` sits at
  `baseRow + n`, so a count reaching past the last row describes a tile no cell
  exists for. A sheet that quietly stopped short of what was asked is worse than
  one that says so: the author would ship believing there were four.
- **`variants` written on the file.** The key means "a family descends from this
  sprite". Set-wide it would read as a count with no base to descend from, so it
  is refused rather than given a meaning nobody wrote.
- **`variants` in the version 1 format.** Its surfaces are bare rectangles with
  no cell to descend from, so the key there is a family nothing could derive.
- **`variants` on any other atlas's sprite.** The object, furniture and agent
  formats have no family column to lay out, and the shared grid loader refuses
  the key in their own vocabulary rather than leaving it unread. Only the sector
  format asks for families at all.
- **A `-Vn` name authored beside its own family.** That name is the derivation's
  to give. A hand-authored `corridor-V1` would collide with the derived tile, and
  which of the two won would come down to map order.

`SectorTileset::variantCount(kind)` answers the declared count for a base type,
and `sectorSurfaceVariantCount(kind)` answers it for a caller that never held the
set. Both answer 0 for a derived tile, which is not the base of a family, and for
anything unknown. A caller sizes a swatch strip from that query rather than from a
constant it would have to keep in step with the tileset.

The placeholder sheet labels every tile with its own name and steps the slab one
shade darker per row, so the family structure reads off the artwork: a variant
that landed in the wrong row shows as a band of the wrong shade rather than only
as a diff. Row 0 keeps the shade the placeholder has always used.

### Wearing a variant

A family is only worth declaring if a World can put its members on the screen.
A Sector's construction record can now say that one of its cells wears a
variant rather than the type's base tile:

```yaml
  - type: room
    name: Room 1
    layer: 0
    y: 1
    x: 0
    cellsWide: 6
    levelsHigh: 2
    topLevelHeight: 0.9
    hasHeightScale: false
    surfaceVariants:
      - column: 2
        level: 0
        variant: 1
```

The entry is `SectorSurfaceVariant { column, level, variant }`, and the
coordinates are **Sector-local**: `column` counts World cells from the
Sector's own left edge and `level` counts them up from its floor line, which
is the same frame the per-cell surface loop resolves a tile in. That is what
lets the entries ride the record. Every structural edit tears the World down
and replays the records, and a move changes only where the record puts the
Sector; entries held in World coordinates would have to be rewritten by that
move to keep pointing at the cells they were drawn for, and one missed by the
rewrite is artwork silently lost. Held Sector-local, the record replays
unchanged and the entries follow the Sector for free.

The entries live on the record as well as on the live Sector for the same
reason in the other direction: a fact held only on the live object would
evaporate on the first resize, and a fact held only on the record leaves the
renderer walking the record list to answer a question it asks once per cell.

### What a resize and a move do to the entries

The entries are anchored to the Sector's own **bottom-left**, and nothing an
edit does renumbers them (#1044). A move changes only where the record puts
the Sector, so a Room or Corridor carries its entries unchanged. Growing the
footprint leftward or upward moves that anchor out from under the artwork and
takes the count with it: the entry at column 2, level 1 is still the entry at
column 2, level 1, and the new cells arrive as base around what was already
painted. Rewriting the entries to hold the artwork on the World cells it was
painted on is the renumbering this rule rules out - it is the one that loses
entries the moment a case is missed.

Shrinking is the only way an entry can be lost, and it is lost with its cell.
An entry naming a cell the new footprint does not have is **dropped**, not
clamped inward: sliding it onto the nearest surviving cell would put artwork
where nobody painted it and leave the author to hunt down every cell that had
been moved under them. The record rewrite drops them before the replay, which
is what keeps the loader's out-of-grid refusal from turning a shrink into a
failed edit. The plan reports the loss - `Drop 2 variant cells outside the
new footprint` - so the author confirms it rather than discovering it, and a
Room shrunk back to no entries saves with no field at all, the same as one
that never wore a variant.

What the loader refuses:

- **An entry outside the Sector's own grid.** A column past `cellsWide` or a
  level past `levelsHigh` names a cell that does not exist. Refusing it is a
  loud miscount; drawing nothing for it is a silent one.
- **Two entries for one cell.** Which variant the cell wears would come down
  to list order, so the loader refuses instead of choosing.
- **The field on a record that draws no tiled surface.** A Background and a
  Facade are authored as flat colour and never ask the atlas for a tile, so
  a variant recorded on one is decoration that could never show. A Door or
  Marker record has no surface at all.

What it does not refuse is a `variant` number the installed artwork has no
row for. The document records what was authored; the renderer decides what a
number it cannot draw looks like, and the answer is the base tile rather than
a hole. That single fallback is also what makes a World repointed at the
version 1 family - which declares no families, and so has no `-Vn` tile
under any name - draw base on every variant cell while keeping its entries,
so repointing it back at a family that has them restores the picture with
nothing rewritten. A base tile that is itself missing still draws the
missing-art magenta: falling back from a variant to a base that is not there
is not falling back to something.

The field is behind World schema version 70. A document written before this
feature existed carries no such field on any record and loads as no variants
recorded, which is exactly what it drew before the feature existed, and a
World that never used variants saves with no empty field written. An entry
below version 70 is refused rather than read as a coincidence.

Nothing about a variant reaches traversal, capacity or simulation state: the
same World saved with variants and saved without them differs by nothing but
the variant block itself. `smoke/persistence/SectorSurfaceVariants.cpp`
pins that along with the round trip and the refusals, and
`smoke/render/SectorSurfaceVariants.cpp` pins what each cell draws - the
variant tile where one is recorded and its own tile behind it, the base
everywhere else, on the same rectangle, at the same crop and the same tint
it had before any cell could be recorded.

Every one of the ten tiles fills its cell: `drawn_size: [1.0, 1.0]`, content
128 x 320 at the authored grid's pixel size, `offset: [0.0, 0.0]`. That is the
whole of this set's content-size story - Infrastructure has no sub-cell art - and
it is what the #1013 spec's dimensions table records for `SectorAtlasNew`
(#1033). The sub-cell sizes live in the Objects set and are tabulated in
[the object atlas doc](object-tileset.md#content-sizes-as-shipped).

Two of those surfaces become live that were declared in the old manifest but never
drawn. The Shuttle and the Staircase used to borrow the Lift and Stairwell tiles,
with comments saying their procedural carriage and steps would fight a fixed tile
underneath. They do not: the procedural geometry renders over the Sector surface
either way, and in this family it renders over the type's own art.

Which surface a Sector type wears is decided by the family the installed atlas was
authored in, not by whether the atlas merely has a tile of that name. Airlock,
Chamber and Dumbwaiter may ask presence, because the old set has no tile under any
of those names and so keeps borrowing the Corridor and Lift art it does have rather
than hitting the missing-tile magenta. Shuttle and Staircase may not: the old sheet
has declared both regions since it was written and never drew them. A presence test
there reads those dormant regions as own art and repaints every World on
`SectorAtlas` - the Shuttle's rail corridor dropping from the Lift's 32x117 shaft
to a 31x46 fragment, and a second fixed staircase appearing underneath the
procedural steps (#1027). `SectorTileset::family` carries the file's `version`
through the install, and `sectorTilesetIsNewFamily()` is what the renderer asks
before it puts a Shuttle or a Staircase on its own tile.

The boundary strips are dropped. A version 2 sector tileset has no `boundaries`
map, and the loader refuses one that declares it, so the dropped artwork cannot
come back quietly. Every Sector edge therefore takes the renderer's primitive line
fallback. That is an artwork removal only: the wall-span calculation, the
partial-height opening handling and the floor-and-ceiling-at-bounds-only rule are
untouched, and the existing draw-order and Facade smoke modules still prove them.

The `ImageSet` entries the application manifest declares for `SectorAtlasNew` are
generated from the tileset, not authored beside it:

```sh
python3 scripts/generate_sector_atlas_new.py --write   # regenerate
python3 scripts/generate_sector_atlas_new.py --check   # verify, as CI does
```

The render smoke module checks the committed manifest, the paired `Image` and the
atlas-set registry entry against the tileset on every run, so a drift fails the
build rather than showing up as artwork in the wrong place.

## The palette's variant sub-strip

A declared family is only usable if the author can see it. Selecting a Sector paint
tool in the editor reveals a sub-strip of that type's swatches beneath the tool:
the base tile first, then `V1` .. `VN`, one swatch per tile the installed atlas
declares (#1041). The count comes from `sectorSurfaceVariantCount(kind)` rather
than from a number the editor keeps, so changing what a base sprite declares
changes the strip and nothing in the editor is touched between one count and the
next. A swatch is never offered without the tile behind it, so there is no empty
slot an author can pick, and a type that declares nothing shows its base alone.

Background and Facade get no strip. They are Sector types authored as flat colour,
they never ask the atlas for a tile, and a variant entry on a record that draws no
tiled surface is refused at load, so a swatch under either would be a choice that
could never be drawn.

Each swatch is the atlas tile itself, framed into the swatch's box with its tile
name over the artwork - which is what keeps the strip readable on a placeholder
sheet and needs no change when real art arrives. Framing is `drawSectorTilePreview`,
which is deliberately not the World's surface draw: that one cuts its area into
World cells, anchors on the Sector's floor line and crops whatever stands above
the ceiling, and a swatch has no cells, no floor and no zoom to be anchored on.
One tile in, one image out, whatever the box.

The strip is a row of the tray rather than a panel floating over the canvas: the
tray grows by exactly the rows the swatches need, the swatches sit below every
row of buttons, and the strip starts under the tool it belongs to and is pulled
back only as far as it must be to stay inside the tray.

## Painting a variant onto a cell

Picking a swatch gives the paint tool a second job (#1042). With nothing picked
the tool does what it always did - dragging on the World creates Sectors. Picking
any swatch, the base included, switches it into *surface paint*, where a click
puts that variant on the one cell under the cursor instead of making anything.
Clicking the swatch that is already picked deselects it and the tool falls back to
create, which keeps create the thing an author returns to and keeps the base
swatch meaning one thing: the base tile.

A click resolves against the document before it changes it. The cell's Sector must
be of the type the swatch belongs to - the same family name the renderer wears the
Sector in - and the cell must be one the World will accept an entry for. A cell of
the wrong Sector type, a cell with no Sector at all, a flat-coloured Background or
Facade, and a cell off the map are all refused in the same shape, and the hover
shows the refusal as a red outline before the click is ever made. The hover and
the click run the same resolve, so the colour cannot promise a paint the click
would refuse.

An accepted click writes two things: the live Sector, which is what the renderer
reads this frame and why the cell changes under the cursor with no rebuild, and
the Sector's construction record, which is what the World saves and what the next
structural edit replays. The base swatch removes the entry rather than storing a
zero, so a cell painted back to its base and a cell that never wore a variant are
the same thing and save the same way - with no field at all. Each accepted click
is one undo step, and undoing restores the document exactly.

## The version 1 draft set

`resources/textures/sectors.tileset.yaml` is the version 1 format: explicit
`[x, y, width, height]` rectangles, because the generated draft is not a uniform
tile grid, and four boundary strips - `floor`, `ceiling`, `left` and `right` -
that compose centre, end, corner and open-wall variants rather than baking every
combination into separate sprites. It declares seven surfaces; Shuttle and
Staircase were among them but were never drawn, because the renderer mapped those
types to the Lift and Stairwell tiles. That is still what they draw: the version 1
loader puts the set in the Old family, and the Old family borrows both, so a World
that has always been drawn on `SectorAtlas` keeps drawing what it drew before the
new family existed (#1027).

The old `SectorAtlas` and `StationAtlas` stay registered and canonical until
nothing references them; `SectorAtlasNew` comes up beside them.

## Generation and installation

The grid resolution the version 2 sector and object tilesets share lives in
`pf-render/src/AuthoredTileGrid.cpp`, so a cell-and-content sheet is read the same
way whichever atlas it fills. Pixel regions still come from the `ImageSet` and
render semantics still come from the authored tileset - the split ADR 0010 as
amended describes.

Whether a base sprite may declare a variant family is a parameter of that shared
loader and not a fact about grids, because it is not the grid that has to keep the
promise. The sector loader passes `Derived` and gets the count validated and
recorded; every other atlas passes `Refused`, which is the default, and a sprite
carrying the key is refused. The derived tiles themselves are named and placed by
the sector loader, which owns the `-V` vocabulary; the shared grid only establishes
that the declared count has room to stand on the declared sheet.

CMake copies both assets beside each executable under `textures/`, including
on builds with no C++ changes. Startup resolves that directory relative to the
executable, not the working directory. Invalid or missing assets produce a
controlled startup error. The GL texture is deleted before the GL context.
Headless rendering without an installed texture retains the primitive fallback.

Run `ctest --test-dir build-linux -R 'sector-tileset|smoke-startup|smoke-render' --output-on-failure`
after building the GUI, headless and `pf-sector-tileset-checks` targets.
