# Sector textures

There is one sector renderer. `pf-render` loads the definition, installs the atlas
as a Willpower `ImageSet` resource, and repeats its regions into the World command
stream; the editor and the shipped game both draw from that same code and see the
same result. The editor keeps no sector rendering of its own, and neither does the
game. See [ADR 0010](adr/0010-render-world-canvases-with-mpp-and-manage-assets-as-resources.md)
and the `pf-render` module header for the boundary this sits behind.

The renderer repeats a surface every 1x1 world unit, magnified to the World cell's
screen size, cropping partial cells rather than stretching a whole tile to a
Corridor's height. The repeat is anchored on the bottom of the Sector's area - its
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
