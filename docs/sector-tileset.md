# Sector textures

There is one sector renderer. `pf-render` loads the definition, installs the atlas
as a Willpower `ImageSet` resource, and repeats its regions into the World command
stream; the editor and the shipped game both draw from that same code and see the
same result. The editor keeps no sector rendering of its own, and neither does the
game. See [ADR 0010](adr/0010-render-world-canvases-with-mpp-and-manage-assets-as-resources.md)
and the `pf-render` module header for the boundary this sits behind.

The renderer repeats a surface every 1x1 world unit, magnified to the World cell's
screen size, cropping partial cells rather than stretching a whole tile to a
Corridor's height. Room interiors therefore support arbitrary height without
intermediate floors. Backgrounds and Facades intentionally retain their authored
opaque colours and take no tile at all - they are authored flat colour by domain
definition, and the renderer fills them without asking the atlas for a surface.
Vehicles, stair and ladder mechanisms, thresholds, other objects, Agents and editor
overlays retain their existing rendering. Aperture clipping and render order are
unchanged; wireframe mode remains primitive-based.

## SectorAtlasNew

`resources/textures/sectors-new.tileset.yaml` is the version 2 authored tileset
behind `SectorAtlasNew`: ten full-cell Infrastructure tiles on the 128x320 grid
at 640x640, one for each of the ten surface-bearing Sector types - `room`,
`corridor`, `ladder`, `lift`, `shuttle`, `stairwell`, `staircase`, `airlock`,
`chamber` and `dumbwaiter`. Every tile is drawn at one full World cell with no
render offset, because the repeat is per World cell; the loader refuses a surface
drawn at any other size or offset off its cell's anchor.

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

CMake copies both assets beside each executable under `textures/`, including
on builds with no C++ changes. Startup resolves that directory relative to the
executable, not the working directory. Invalid or missing assets produce a
controlled startup error. The GL texture is deleted before the GL context.
Headless rendering without an installed texture retains the primitive fallback.

Run `ctest --test-dir build-linux -R 'sector-tileset|smoke-startup|smoke-render' --output-on-failure`
after building the GUI, headless and `pf-sector-tileset-checks` targets.
