# Object atlas

The Objects atlas the game ships is `resources/textures/objects-new.png`, a
768x1280 RGBA sheet laid out on a uniform 128x320 authored grid: one cell to a
World cell rather than a 64x64 square of screen pixels. The World cell's screen
size is a separate fact: the renderer stretches each resolved region into the
World-unit rectangle it is drawn into, so the artwork magnifies with the cell
instead of being re-cut (#1007). Unoccupied cell space is transparent. Small
objects do not expand to fill their cell.

The version 1 sheet this replaced - `resources/textures/objects.png`, a 320x960
atlas on a 64x160 grid - was deleted when the #1013 transition ended (#1024).
Its tileset file, `objects.tileset.yaml`, survives because the custom theme sheets
share its vocabulary: `StationFurnitureAtlas` and `DoctorFemale1Atlas` author
their semantics against it. Its `image` field names the sheet those rectangles
were originally drawn against; nothing reads that field at runtime, because the
install takes pixel regions from each theme's own `ImageSet` resource.

`objects.tileset.yaml` records each sprite's cell, local content rectangle and
whether its RGB can be tinted. The manifest's `tile_size` is the authored grid:
how the artist laid the image out. The loader requires only that whole authored
cells cover the image with no remainder; it never compares that grid with the
World cell, because a World unit's screen size is a different fact and the
renderer stretches a resolved region into whatever destination it is drawn into
(#1006). A content rectangle that escapes the image, or that does not start
inside its own authored cell, is refused. Content rectangles exclude alpha
padding and may run from their own cell into a neighbour, as the wide Lying tile
does. Alpha padding never changes collision, selection or visual sizes.
The Agent and Marker sprites have grayscale RGB. Agents use the existing resolved
Agent colour, including tag overrides and selection, as the vertex tint carried on
the recorded command. Existing height scaling, horizontal centring and foot
position are preserved.

## Render semantics: offset, drawn size, tintable

A tileset file carries a `version`. Version 1 - what `objects.tileset.yaml` is -
records a cell, a content rectangle and a `tintable` flag. Version 2 adds the two
facts a version 1 renderer had to guess, per tile:

- `offset` - the render offset, in World units, from the tile's anchor. `+x` is
  right and `+y` is up, so a negative `y` hangs artwork below the anchor.
- `drawn_size` - the drawn size, in World units. One World unit of authored art is
  one authored tile, so a drawn size larger than the whole image cannot be drawn
  from it and is refused.

Both are required in a version 2 file and both must be finite. A missing field, a
`.nan` or an `.inf` is a load refusal rather than a silent default, because a
render offset that quietly became zero would put artwork where no author said it
goes. A drawn size that is zero, negative, non-finite, or larger than the image is
refused the same way. A version 1 file declares no render semantics, so the loader
gives it the format's own identity: no offset, and a drawn size of whatever the
content rectangle measures in World units over the authored grid.

`tintable` is the only source of tintability. The C++ rule that used to derive it
by matching a tile's name against `agent`, `human-*`, `robot-standing` and
`cleaning-bot-standing` is gone: a new Agent type can no longer become tintable by
accident, and a tile named without a hint can no longer fail to be.

The offset and the drawn size belong to the artwork and never to the domain object.
CONTEXT.md keeps that line - Support elevation is a physical fact and is explicitly
not a render offset - and putting a visual extent on the object would collapse it.
A Walkway is `Object(cellX, cellY, 1.0, 0.0)`: deliberately zero height so it
never obstructs pathing, routing or support, while its `walkway` tile draws a
24px slab at `offset: [0.0, -0.075]` and `drawn_size: [1.0, 0.075]`, hanging
that far below the floor line of the cell it was authored in.

## Variant families

A version 2 object sprite may declare a variant family with one number:

```yaml
  door:
    cell: [0, 0]
    variants: 3
    content: [0, 0, 128, 320]
    offset: [0.0, 0.0]
    drawn_size: [1.0, 1.0]
    tintable: true
```

That count is the whole of what is authored about the family. The loader expands
it into `-V1` .. `-VN`, each in the base's own column one row below the tile
above it - `-V1` directly beneath the base, `-VN` N rows down - and each
inheriting the base's content rectangle, render offset, drawn size and tintable
flag (#1053). Adding a variant is a picture decision and never a re-declaration
of geometry, so a variant cannot be given art that does not line up with the base
it is a variant of, and no author writes a name or a cell that the rule could
contradict. The base keeps its bare name and stays variant 0, so every tile name
that existed before families still means what it meant.

This is the promise the sector tileset format has made since #1038, kept by the
same code rather than by a second copy of it. The naming, the placement, the
sheet-fit check and the refusals all live in `pf-render/src/AuthoredTileGrid.cpp`,
which both loaders read through; each loader only says whether the format it is
reading derives families. The Object, Agent and Furniture atlases are all written
in the object format and all take the capability with it, and their generators
derive the same tiles under the same refusals - which matters because the install
pairs from the manifest inward, so a family the sheet showed but the manifest
left out is a family nothing installs.

What the format refuses, at load:

- **A family that runs past the bottom of the declared sheet.** `-Vn` sits at
  `baseRow + n`, so a count reaching past the last row describes a tile no cell
  exists for. The refusal names the base's row and the sheet's row count,
  because "it does not fit" without both numbers is not something an author can
  act on.
- **A count that is not a whole number.** `two`, `1.5`, `true` and `[1]` are not
  the author stating a count, and rounding one into a family - or dropping one -
  is the loader inventing what the file did not say. A negative count describes a
  family climbing above its base, which is not what the column means.
- **A `-Vn` name authored beside a family.** That name is the derivation's to
  give. A hand-authored `door-V1` would collide with the derived tile, and which
  of the two won would come down to file order. The reservation covers the whole
  `-Vn` shape rather than only the cells one family happens to occupy, because a
  second family could be declared beside it in the next edit.
- **`variants` in the version 1 object format.** A version 1 tile authors no
  render semantics at all - its drawn size is whatever its content rectangle
  measures over the grid - so a family stacked beneath such a base is a family
  nobody sized. The key is refused rather than quietly derived, because an author
  who wrote it meant to gain the capability and would not have gained it.

The shipped object, agent and furniture sets declare no family yet. The
capability lands alone, ahead of the Door leaf artwork that will use it, so the
format change is proven before any artwork depends on it (#1053).

## ObjectAtlas

`resources/textures/objects-new.tileset.yaml` is the version 2 authored tileset
behind the Objects atlas, laid out on the 128x320 World cell at
768x1280. The Walkway, the Bulkhead Door, the Access panel, the Door, the three
Window styles, the Booth Window's frame and shutter, the two Button states and
the Marker are live in it, along with the three vehicles - the Lift car, the
Platform lift and the Shuttle carriage - and the rest of the sheet
drops in against the same image size. Since the #1013 terminus this is the only
Objects atlas: the merged `objects.png` sheet and the old-family name it was
installed under are both gone (#1024).

The set holds 18 tiles in the 24 cells of that 6x4 grid. The vehicles are what
took the sheet from three rows to four. They arrived when the set held nineteen
tiles, and cramming two into one cell would break the one promise the grid makes
to whoever draws the real art: that a tile owns its whole 128 x 320 slot. The row
has stayed since #1031 dropped the Booth Window's closed panel, which left one
cell free: pulling the last tile up into it would move every other tile's pixels
to buy nothing, and the image is sized for the art that has not landed yet.

The `ImageSet` entries the application manifest declares for `ObjectAtlas` are
generated from the tileset, not authored beside it:

```sh
python3 scripts/generate_object_atlas_new.py --write   # regenerate
python3 scripts/generate_object_atlas_new.py --check   # verify, as CI does
```

The render smoke module checks the committed manifest against the tileset on every
run, so a drift fails the build rather than showing up as artwork in the wrong
place.

Every content rectangle in the set is the tile's declared drawn size measured at
the authored grid's pixel size, rounded to whole pixels: a tile drawn 0.8 x 0.5
World units is 102 x 160 px on this 128 x 320 grid. Art and declared extent stay
in the same proportion, so nothing is stretched on its way to the screen.

### Content sizes as shipped

The table is the shipped geometry for every tile in the set: the World-unit drawn
size the tile declares, the content rectangle that size measures at the 128x320
grid, and where the drawn size comes from - the core geometry constants the
Objects contract (`requiredObjectTiles()`, `pf-render/src/ObjectTileset.cpp`)
composes it from, or the authored reference extent where the domain fixes no
geometry of its own. #1033 reconciled it against the #1013 spec, which carried
three sizes that no code ever built.

| Tile | Drawn size (World units) | Content px | Basis |
|---|---|---|---|
| `walkway` | 1.0 x 0.075 | 128 x 24 | the slab's own thickness, drawn at `offset: [0.0, -0.075]` below the cell's floor line |
| `bulkhead-door` | 0.2 x 0.7 | 26 x 224 | `CORE_BULKHEAD_DOOR_WIDTH` x `CORE_CORRIDOR_HEIGHT` |
| `access-panel` | 0.5 x 0.25 | 64 x 80 | the default `AccessPanelGeometry`; a panel authored at another size draws at its own rect |
| `door-L0`, `door-R0`, `door-tall-L0`, `door-tall-R0`, `door-lift-L0`, `door-lift-R0`, `door-shuttle-L0` | 1.0 x 1.0 | 128 x 320 | one full cell: the aperture clip, not the drawn size, bounds a leaf, so one tile serves every Door width and each family differs by the doorway it is clipped by rather than by what it declares |
| `button-enabled`, `button-disabled` | 0.1 x 0.04 | 13 x 13 | `CORE_BUTTON_SIZE` x `CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO` - square on screen |
| `window-clear`, `window-frosted`, `window-tinted` | 0.8 x 0.3 | 102 x 96 | `1 - 2 * CORE_WINDOW_X_INSET` x `CORE_WINDOW_HEIGHT` |
| `booth-window-open` (the frame) | 0.8 x 0.3 | 102 x 96 | the same one-cell Window extent: the frame is the Window |
| `booth-window-shutter` | 0.6461538 x 0.2375 | 83 x 76 | the frame's inner aperture - 42/52 of its width and 38/48 of its height, the frame's wall taking 5 of those 52 px across and 5 of the 48 down on each side |
| `marker` | 0.25 x 0.1375 | 32 x 44 | the authored art's own reference extent; the icon's on-screen size is screen-space (`MarkerIconSize`) |
| `lift-car` | 0.9 x 0.55 | 115 x 176 | the car's own extent: the cell less `CORE_LIFT_CAR_BORDER` on each side, at `CORE_LIFT_CAR_HEIGHT` |
| `platform-lift` | 1.0 x 0.05 | 128 x 16 | the slab's own thickness, drawn from its own anchor |
| `shuttle-L0`, `shuttle-M0`, `shuttle-R0` | 1.0 x 0.55 | 128 x 176 | one cell wide at `CORE_SHUTTLE_CAR_HEIGHT` |
| `shuttle-connector` | 1.0 x 0.55 | 128 x 176 | the same extent, drawn at the join between two carriages |

Only the Door leaves are a full cell. The cell is the slot the grid gives the art to
sit in; what a tile declares is the extent it actually draws at, which is why a
Button is 13 x 13 px inside a 128 x 320 cell and a Lift car is 115 x 176 rather
than the 128 x 320 the shaft it runs in fills. A leaf is the exception on
deliberate purpose: it is drawn at its whole cell and translated under the
doorway clip, so the clip - not the declared extent - is what keeps it inside the
opening, and one tile answers every width and each height class (#1056, #1058).
The #1013 spec first wrote the two
vehicle entries as 128 x 320 because it was describing one cell of a transport's
route rather than the vehicle; the renderer draws a car into the car's own live
shape and the Objects contract checks the declared size against the core constants
behind it, so the spec now states the extent and not the slot.

The Button is the entry where the spec was wrong rather than loosely worded. It
read 12 x 32 - a tall button. `core/src/Button.cpp` builds the physical shape as
`CORE_BUTTON_SIZE x CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO`, which is 0.1 x
0.04 World units and 12.8 x 12.8 px: a square. The domain has no tall button, so
the spec moved to the geometry and the sheet did not move at all.

### One Door tile serves all four opening styles

A Door has one tile. The opening style decides which way the leaf crops, not which
artwork is used, so a sliding leaf crops rather than squashing into the strip of
aperture it still covers:

| Style | What the leaf keeps |
|---|---|
| `OpenUp` | the top of the aperture and the bottom of the art |
| `OpenLeft` | the left of the aperture and the right of the art |
| `OpenRight` | the right of the aperture and the left of the art |
| `OpenApart` | two half leaves, one wearing each half of the tile, splitting at the centre |

The crop is driven by the Door's own open fraction, read from the live device
state on every frame rather than from a frame counter, which is what makes a Door
half open show exactly half open however long it took to get there. Fully open,
nothing of the leaf is left and no geometry is emitted - on `OpenApart` that means
both leaves, not one.

A side-sliding leaf's visible edge is pinned at two points (#1061): on the far
jamb at 0% open, so the reveal begins the instant the Door moves rather than a
tenth of the travel later, and on the near jamb at 100%, so a fully open Door
leaves nothing painted across either jamb. Between the pins it moves at a uniform
speed, covering exactly one aperture width. The leaf's own cell puts its trailing
edge one `CORE_DOOR_X_INSET` outside the far jamb, so the animation vector starts
at that inset rather than at zero, and the closed leaf sits that inset further into
its tile: `u[0.2,1.0]` of `door-L0` for a 1-wide `OpenLeft` Door, mirrored to
`u[0.0,0.8]` for `OpenRight`. Both bands still hold the `u = 0.5` symmetry axis
the `OpenApart` split requires.

The declared drawn size is the regular one-cell Door's own extent: `cellsWide -
2 * CORE_DOOR_X_INSET` across a regular Door's `CORE_DOOR_HEIGHT`, which is 0.8 x
0.5 World units. As with the Access panel, the object's own geometry is what the
artwork is drawn into; the declared size is the reference the art is authored
against, not a cap on what the object may be.

### The Booth Window is a fixed frame with a shutter behind it

A Booth Window composes two tiles rather than switching between two states of
one. `booth-window-open` is the frame - opaque at its edges, transparent through
its centre - and it never moves. The shutter is the opaque panel that slides
behind that frame, and the frame's transparent centre is what makes the slide
visible. Fully closed, the shutter fills that centre, so the closed state is
already drawn and needs no tile of its own. The set used to carry a
`booth-window-closed` panel for "the artwork path that wants one tile for the
closed state"; no such path existed, the Objects contract required the tile, and
nothing drew it - so #1031 dropped it rather than inventing a consumer for art
nobody asked for.

The shutter's declared drawn size is the frame's transparent centre rather than
the frame: 42/52 of the frame's 0.8 across and 38/48 of its 0.3 down, which is
0.6461538 x 0.2375 World units. The renderer clips it to that same inner rect,
so the shutter can never paint over the frame it retracts behind, and any
enclosing aperture clip intersects with it as it travels.

### Buttons carry their state in the art, Markers carry theirs in the tint

A Button's two states are two tiles rather than one tile and two tints, so the
state is legible from the artwork alone at the 12.8 x 12.8 px the control
actually occupies. Both are authored untintable: the renderer's enabled green,
disabled grey and Broken red are wireframe and fallback facts, and letting them
multiply into the art would wash out the difference between the two tiles the
state is carried by.

The Marker is the one tile in the set that takes a tint. Its icon is grayscale so
the gold the renderer carries multiplies into it, and that gold is
`core::SelectedAgentColour` - the same colour the Agent selection highlight
resolves to - which lets a destination read as part of the Agent colour
vocabulary rather than as a fixed decal. The icon's on-screen size is a
screen-space fact (`MarkerIconSize`, a fixed pixel longest edge that does not
follow the World cell), so the Marker's declared drawn size is the authored art's
own reference extent rather than the destination it is drawn into.

### The Bulkhead Door draws its own slab, not a cell

A Bulkhead Door is a solid physical slab straddling the boundary between two
adjacent Locations. Its geometry puts it at `cellX + 0.9` to `cellX + 1.1`:
0.1 into one cell and 0.1 into the next. Its `bulkhead-door` tile declares
`drawn_size: [0.2, 0.7]` at `offset: [0.0, 0.0]` from the door's own anchor,
so the artwork crosses the cell boundary exactly where the collision does, and
is drawn at the slab's 0.2 x 0.7 aspect ratio rather than the cell's 2:5. The
tile's content rectangle is 26 x 224px, the slab at the authored grid's pixel
size.

The slab opens vertically from the floor, so what is visible is what is still
down. The renderer draws the top `1 - openPercentage` of the declared extent
and crops the artwork's source by the same fraction, so the art slides out
through the top edge instead of squashing into the strip left below it. The
fraction is read from the live device state on every frame, never from a frame
counter, which is what makes a door half open show exactly half open however
long it took to get there. Fully open, nothing of the slab is left and no
geometry is emitted.

### The Access panel draws at its authored geometry

An Access panel sits on the visible back surface of a Room, Corridor or
Facade, and its geometry - `width`, `height` and the `yOffset` it sits above
the cell floor - is authored per panel. The panel's own authored rect is
therefore what the artwork is drawn into; the `access-panel` tile's declared
drawn size of 0.5 x 0.25 is the reference the art is authored at, which is
the default geometry, and a panel authored 1.0 x 0.5 draws 1.0 x 0.5. Its
content rectangle is 64 x 80px.

The leaf retracts into the top edge as the panel opens, and the renderer crops
the tile the same way it crops the Bulkhead Door's slab: the top edge stays
where it was, the bottom rises with the live progress, and the revealed part of
the panel shows the recess behind the leaf.

### The Shuttle carriage is composed from role tiles

A carriage is drawn from three tiles - `shuttle-L0`, `shuttle-M0`, `shuttle-R0`
- and the only difference between a three-cell carriage and a four-cell one is
how many times the middle gets reused. That is the whole value of putting the
role in the name: the end-versus-middle difference is carried by the tile rather
than by the width, so widening a carriage buys cells and never tiles. A two-cell
carriage is `shuttle-L0`, `shuttle-R0` with no middle at all; a four-cell
carriage is `shuttle-L0`, `shuttle-M0`, `shuttle-M0`, `shuttle-R0`.

Each carriage cell declares `drawn_size: [1.0, 0.55]` - one cell across
`CORE_SHUTTLE_CAR_HEIGHT`, 128 x 176 px on this grid - and the renderer draws
it into that cell of the carriage's live shape. The role vocabulary is the same
one `FurnitureAtlas` speaks further down; a carriage cell is one cell wide at
one height level, so `L0`, `M0` and `R0` say everything there is to say about
where a cell sits in the train.

The coupling between two carriages keeps a bare name, `shuttle-connector`, with
no role suffix. It is drawn at the join: the one-cell gap that separates one
carriage from the next and belongs to neither. A role name would claim it was a
cell position on a carriage and invite someone to compose it into a carriage's
own cell count, which would put a coupling in the middle of a train.

The renderer prefers the role tile and falls back to the pre-convention
`shuttle-car-left`/`-middle`/`-right`/`-connector` names when the installed set
carries no role tiles, in the same shape as the Sector surface fallback. Adopting
the convention is therefore an upgrade an older sheet survives rather than a
break it takes.

### The Lift car and the Platform lift follow the moving thing

Both are drawn into their own live shape rather than into the shaft they run in.

The Lift car's `lift-car` tile declares `drawn_size: [0.9, 0.55]`: the cell
width less the `CORE_LIFT_CAR_BORDER` the car is inset by on each side, across
`CORE_LIFT_CAR_HEIGHT`. The renderer draws it into `Lift::getCurrentShape`, so
the sprite fills the cell the car occupies and travels with the car up the shaft.

The Platform lift is a slab rather than a car: `platform-lift` declares
`drawn_size: [1.0, 0.05]` - one cell across its 0.05 thickness, 128 x 16 px -
and is drawn into the slab's live bounds. The shaft the slab runs in is drawn
separately, as its own subdued outline, precisely so the two are not confused:
a slab halfway up a four-Level shaft is drawn halfway up.

### The Force Bridge is given no tile, deliberately

A Force Bridge's span is continuous and arbitrary - any fraction of a cell
over any gap - so a fixed tile would be stretched wrong at most lengths it is
used at, and the 2px line is honest about being an energy field. A version 2
object tileset that declares a `force-bridge` tile is refused by the loader, so
the decision cannot be reversed by an edit to a sheet nobody thought about.

## FurnitureAtlas and the tile naming convention

`resources/textures/furniture-new.tileset.yaml` is the version 2 authored tileset
behind `FurnitureAtlas`: eleven full-cell tiles on the 128x320 grid at 640x960 -
`chair` and `chair-L0`, and `sofa`/`desk`/`bed` each at `-L0`, `-M0` and `-R0`.
Every one of them is drawn at one full World cell with no
render offset, so a piece one cell wide covers one cell and a piece four cells wide
covers four. `scripts/generate_furniture_atlas_new.py` generates its sheet and its
manifest entries the same way the Objects atlas's does; both scripts share
`scripts/atlas_new.py`.

A Furniture tile name is `<piece>-<role><level>`. The role is `L`, `M` or `R` -
the left cap, a middle cell, the right cap - and the level is the height level
within the piece's footprint. The anchor is the **bottom-left** of that footprint,
so every offset is non-negative and no sign has to be encoded in a name, and one
digit per axis carries the offsets 0 through 9 - which caps a piece at ten cells
per axis, ten columns and ten rows. An eleventh column cannot be named, and the
loader refuses it. The other vocabulary is `-XY`, an absolute offset from the
anchor, retained as the escape hatch for the irregular footprint a role
vocabulary cannot name.

The invariant the loader enforces is that `L` is always present because it is the
anchor, `R` appears when the piece is at least two cells wide, and `M` appears when
it is at least three. For height > 1 every role present must be present at every
height level, so a footprint can never be half-specified. A piece speaks one
vocabulary throughout: a footprint mixing role-suffixed and absolute-suffixed
tiles is refused, and so is a bare name inside a footprint that names its other
tiles by position.

The suffix is applied only where the piece can span more than one cell. Furniture
is that kind of thing, so its role tiles all carry a suffix and a width-1
chair falls out of the rule as `chair-L0` rather than being a special case; a
single-cell object that can never span more than one cell - a Button, a Marker -
keeps its bare name and gains no suffix carrying no information.

**The catalogue's per-tile `{x, y}` stays authoritative for placement.** The
suffix earns its place by making the art sheet self-describing, and the loader
asserting agreement is what stops that becoming a lie: `sofa-R0` placed at a
position a piece's width forbids, a `-Ly` name on a tile the catalogue places at
another level, and an absolute name whose digits do not match its offset are each
refused with the tile named. `core/src/FurnitureTileNaming.cpp` is where both
vocabularies are parsed and checked, and `FurnitureCatalogue::readFile` asks it
about every definition's whole footprint.

Names that carry no recognized suffix - `sofa-left`, `button-enabled` - load
exactly as they always did. The convention binds a piece that speaks it, not
every catalogue written before it. The one bare name the Furniture sheet carries
art for is `chair`: the cell a piece speaks when it names no position at all,
which is what closed the `layouts.furniture.lua` `larger` gap this sheet used to
record as uncovered.

### The shipped catalogues were repointed (#1029)

Registering the role tiles was only half the migration. Every shipped catalogue
under `resources/test-worlds/` still named the pre-convention sheet - `chair`,
`sofa-left`, `desk-right` - and `FurnitureAtlas` carries none of those names,
so any World pairing the atlas a new World now defaults to with a shipped
catalogue drew every Furniture piece as the missing-tile magenta. The shipped
catalogues now speak the convention: `chair-L0`, `sofa-L0`/`sofa-R0`,
`bed-L0`/`bed-R0`, `desk-L0`/`desk-R0`, and the four-cell composition piece
reuses `sofa-M0` twice rather than needing a fourth tile.

Two things had to come with that repoint.

**The old sheets are gone.** Before the #1013 terminus a World that had not moved
to `FurnitureAtlas` drew the merged `objects.png` sheet, and the repoint had to
change not a pixel of it, so `FurnitureAtlas` and `StationFurnitureAtlas` each
carried the role names over the identical pixel region as the old name they stood
for. The old sheet was deleted with #1024 and those aliases went with it: there is
one Furniture atlas now, and it carries its own art.

**A footprint the sheet cannot serve is a stated gap.** A piece whose offsets the
role vocabulary and the `-XY` escape hatch both fail to name has to be written
down rather than left to render as a missing tile. `uncovered_footprints`
at the top of `furniture-new.tileset.yaml` records each one with the reason, in the
same shape as the Agent sheet's `reserved_appearances`. A record with no reason
is refused, as is one naming a piece the sheet now covers: a gap that is written
down is a decision, and one that is not is a magenta rectangle nobody noticed.
The `larger` piece in `layouts.furniture.lua` was the last entry; widening the
sheet by one row for the bare `chair` cell covered it, and the record went with it
(#1024).

The `render` smoke module's `furnitureAtlasNew/*` checks prove all of this: the
ten tiles registered at full-cell drawn size, a three-cell sofa rendering from
`sofa-L0`, `sofa-M0` and `sofa-R0`, a four-cell sofa reusing the same middle
tile across two cells and naming three tiles in all, and each refusal at the
loader seam. The four added with #1029 read the shipped bundle itself rather
than a fixture made to conform - which is the whole point, since a synthetic
catalogue cannot see that the real one names something else:

- `everyShippedFurnitureCatalogueResolvesInEveryNewFurnitureAtlas` pairs every
  tile of every shipped catalogue with the Furniture atlas the atlas-set
  registry names, and a disagreement names the catalogue, the piece and the tile.
- `noShippedFurnitureRendersMagentaUnderTheNewDefaultAtlas` renders each shipped
  piece on its own under `FurnitureAtlas` and requires its artwork, then pulls
  the atlas out from under the same piece to prove the magenta detector is not
  deaf.
- `everyShippedFootprintIsCoveredOrRecorded` holds the covered-or-recorded rule
  and refuses a gap record that is reasonless or has stopped describing a gap.

## AgentAtlas and the appearance-keyed grid

`resources/textures/agents-new.tileset.yaml` is the version 2 authored tileset
behind `AgentAtlas`: four Agent appearances, each carrying all five Poses,
twenty tiles on the 128x320 grid at 640x1280. `scripts/generate_agent_atlas_new.py`
generates its sheet and its manifest entries the same way the other `New` atlases'
does; all of them share `scripts/atlas_new.py`.

The atlas is keyed by distinct **appearance**, not by Agent type. Human and Remote
operator are one appearance, so both name the `human-*` tiles and neither gets its
own copy of five tiles; a new type that looks like an existing one names that
appearance's row, and only a genuinely new look costs five new tiles. The fourth
row is the generic `agent` appearance, the fallback a type with no artwork of its
own names. It is a row like any other, so the fallback is drawn by the same path
as the real appearances rather than by a special case in the renderer. Every
appearance gets all five Poses authored even where that appearance can never
perform them - a Cleaning Bot never Lies down, an Android never Crawls - so the
grid stays uniform and an artist always knows which cell holds which Pose.

Appearance is the row and Pose is the column, and that is forced rather than
chosen. Lying and Crawling are 144px wide - more than one 128px cell - and a
content rectangle must start inside its own cell and fit inside the image, so a
row of four over-wide tiles cannot be laid out in four columns at all: the last
one runs off the right edge. With the appearances down the rows, every over-wide
Pose has a neighbouring cell to overhang into. The column order is standing,
sitting, lying, crawling, crouching rather than the domain's canonical Pose order
for the same reason: the tile following each over-wide one is inset by exactly its
overhang - 16px after Lying, 32px after Crawling - so no two tiles' art ever
overlaps, and the last column is a one-cell-wide tile, which is what keeps the
widest tile inside the image.

Every tile is authored `tintable: true`. Agent art is grayscale, and the vertex
tint is the resolved Agent colour - the Agent's own colour, a Colour tag's
override, or the selection gold - which multiplies into it faithfully. The
`render` smoke module's `agentAtlasNew/*` checks prove the twenty tiles
registered as a full cross product, two types sharing one row, the generic row
standing in for a type with no artwork, the Agent colour and its tag override and
the selection highlight all still driving the tint, and the over-wide Lying and
Crawling tiles resolving across adjacent cells rather than being cut back to one.

### Every appearance row is worn, or says why it is not

Keying by appearance means a row exists independently of anything wearing it. An
unworn row is artwork somebody drew that nothing will ever draw, and nothing
notices it missing because nothing asks for it. That is exactly what happened:
#1018 authored the `android-*` row and never repointed the shipped Android type,
which went on naming `robot-standing` - a tile this sheet has never carried - so
under the atlas #1023 made the default every Android Agent drew the missing-tile
magenta (#1028).

The shipped Android type now names `android-standing`, and the pairing is asserted
rather than hoped for:

- every shipped Agent type's declared Pose tiles resolve in the Agent atlas the
  atlas-set registry names, and a failure names the type and the tile;
- every appearance row the sheet authors is named by at least one shipped Agent
  type, or is listed under `reserved_appearances` in the tileset with the reason it
  is held back. The generic `agent` row is reserved on that basis - it is the
  fallback a type with no artwork of its own names, and no shipped type needs it.
  A reservation with no reason is refused, and so is a reservation naming a row the
  sheet no longer authors, because a stale reservation is where the next orphan
  hides.

The shipped types are discovered from the bundled `.agent.lua` resources rather than
listed by hand, so a type added to the bundle is paired against the atlas the day
it lands. The old Agent sheet that carried `robot-standing` was deleted with the
#1013 terminus (#1024); `AgentAtlas` is the only Agent artwork the bundle ships.

## The install seam

`installAtlasTiles()` in `pf-render` is the one place the two authorities meet,
and every viewport atlas installs through it:

- **Pixel regions come from the `ImageSet` resource.** It stays the runtime
  pixel-region contract ADR 0010 names, and it is the only place a drawn region is
  taken from.
- **Render semantics come from the authored tileset** the atlas is asked about.

Which tileset that is is stated in `resources/atlas-sets.yaml`, the authored
atlas-set registry: one entry per atlas, giving the tileset file and the
authored `kind` of that tileset (`sector` or `object`), resolved against the
registry. The install checks the declared kind against the slot it is filling,
so a mismatch is reported rather than silently loading the wrong shape.

The registry is a separate file because the manifest cannot carry the binding -
the vendored Willpower `ImageSet` schema fixes the Options an `ImageSet` is
allowed to carry (`filtering`, `uv-style`, `colour-space`, `wrapping`,
`mipmaps`), and extending the vendored SDK was rejected as a much wider blast
radius than the atlas work. The registry adds the binding without touching the
resource layer: the tileset stays the single authority for what a tile means,
and the registry only says which tileset is asked about for which atlas. The
render smoke module checks that every `ImageSet` in the manifest is covered by
the registry, that every named tileset loads, and that it defines every tile
the atlas declares, so the mapping cannot rot quietly.

A tile the `ImageSet` declares that the tileset never authored is refused, as is a
pixel region escaping the atlas image. Nothing in the install derives a tint from
a name or a drawn size from a domain object's geometry.

### The Objects atlas is a World-level choice, and is checked at load

A World selects the atlas each of its four viewport slots draws from, and the
Objects slot is one of them (`World::objectImageSetResourceName`, persisted as
`objectImageSet` from schema version 69). Its default is `ObjectAtlas`, the set
every World drew from before the choice existed, so a document that never names
one is drawn exactly as it was.

Because the renderer takes a tile's drawn size and render offset from the
tileset rather than from a cell-sized quad, that choice cannot be trusted the way
the Sector, Agent and Furniture choices can. A Door's artwork drawn at an extent
that disagrees with the Door's shape lands outside the collision that stops the
Agent, which is a worse failure than no artwork at all. So the Objects slot is
checked against a contract before it is installed
(`checkObjectAtlasContract`, `pf-render`):

- every tile the object renderer draws from the slot must be present;
- each must declare the drawn size the renderer draws it at, composed from core's
  own geometry constants where the domain fixes one - the Door leaf is the
  doorway less `CORE_DOOR_X_INSET` at `CORE_DOOR_HEIGHT`, the Lift car the cell
  less `CORE_LIFT_CAR_BORDER` at `CORE_LIFT_CAR_HEIGHT`, the Shuttle cells at
  `CORE_SHUTTLE_CAR_HEIGHT`, the Button `CORE_BUTTON_SIZE` over the cell's y:x
  render ratio - and the authored reference extent where it does not, which is
  the case for the screen-space Marker icon and for the Walkway and Platform lift
  slab thicknesses;
- each must declare the render offset the renderer draws it at, which for the
  Walkway is the whole of its placement: its physical shape is a zero-height
  line, so the offset is the only thing hanging its slab on the floor.

The contract is held in both directions. `requiredObjectTiles()` is the list it
is written against, and the render smoke module reads every source that names an
Objects tile to a draw call - `pf-render/src/Render.cpp` and `editor/src/UI.cpp`
- and requires every entry in that list to be named by a draw call: directly, as
an argument to one, or as the arm of the conditional that picks the name for one.
A tile named only in a comment does not count, which is the point: that is exactly
how `booth-window-closed` read as used while nothing drew it (#1031). Without
that check the list can only ever grow, because adding a requirement costs the
person adding it nothing and dropping one takes someone who happens to notice.

The other direction reads the same sources the other way round: every tile name
literal a draw call asks the slot for must be a tile the installed atlas carries,
and a name that does not resolve fails with its file and line (#1064). This is
the direction that matters most precisely because `drawObjectSprite` returning
false is a designed fallback - the caller keeps its primitive fill, the editor
keeps its font glyph, and the picture degrades quietly. A tile deleted from a
sheet therefore leaves every call site behind unseen unless something asks. Two
things keep the widened scan honest: a source that names tiles without being in
the scanned list fails its own check, and the version 1 arm of a role-or-legacy
fallback - exempt from the version 2 contract, because the renderer only reaches
it when a version 1 set is installed - is still required to be a name the
version 1 set actually carries.

The editor's palette reads the same contract from its own side. Its Door slot
draws a tile rather than a glyph, so the render smoke module reads the icon's
own draw call and requires the tile it names to be one the shipped atlas
carries and one `requiredObjectTiles()` covers, and requires the icon to keep
its font-glyph fallback guarded on the sprite call's answer (#1063). That
fallback is what the custom version 1 theme sets draw for the slot, since
neither carries a door leaf, which is why the guard is pinned as well as the
tile: an icon that stopped testing the call would leave an empty slot under
those themes instead of the glyph it drew before the tile existed at all.

A set that fails is not installed at all. At stack construction the failure
throws, so the game refuses to start on an atlas that cannot carry its geometry;
at a live selection in the editor it lands in the slot's diagnostic and the slot
draws magenta, which is the same recoverable shape a missing atlas takes. Either
way the message names the atlas, names every offending tile, and gives the
declared extent beside the expected one, so it can be acted on rather than
guessed at.

The game selects all four slots from one table (`worldAtlasSlots`,
`PromethiumFermide/render/WorldCanvas.cpp`) every frame, because the World
owns the choice and the game owns nothing but handing all of it over. One
list rather than four separate calls is what makes a slot left out a gap in
a checked list rather than a line nobody counted - which is how the game
came to draw the old Objects sheet while the World asked for
`ObjectAtlas` (#1026). The game has no artwork panel to show a refusal
in, so its render pass writes every slot's diagnostic to the application
log once per distinct value rather than dropping it, and forgets that value
when a pass comes back clean - so a bad atlas that comes back after a fix
is reported again instead of being silenced by what the log already
remembered.

The contract applies only to a tileset that declares its own render semantics -
version 2. A version 1 set derives them from each tile's content rect, makes no
declaration of its own, and is the artwork every existing World already draws;
checking it would refuse the default.


There is one object renderer. `pf-render` loads the definition, installs the atlas
as a Willpower `ImageSet` resource, and emits the sprite quads into the World
command stream; the editor and the shipped game both draw from that same code and
see the same result. Neither application keeps an object renderer of its own, and
the renderer itself contains no ImGui code - the tint is a packed colour on the
command, not an ImGui vertex type. See
[ADR 0010](adr/0010-render-world-canvases-with-mpp-and-manage-assets-as-resources.md)
and the `pf-render` module header for the boundary this sits behind.

Supported sprites:

- Door leaf (all four opening styles): source UV cropping preserves sliding
  rather than compressing the sprite. Fully open leaves emit no geometry.
- Enabled/disabled Buttons, at the existing Button bounds.
- Clear, frosted and tinted Windows: back-sector rendering precedes the overlay.
  Clear glass has a transparent centre; tinted glass is translucent.
- BoothWindow frame and distinct inner shutter: fixed ordinary one-cell Window
  geometry. The transparent Open frame stays stationary and the opaque shutter
  translates upwards behind it, clipped to the frame's centre. The shutter is its
  own `booth-window-shutter` tile and the closed state is that shutter fully
  closed, with no closed panel in the set - the 42x38 subregion of a Closed tile
  that the deleted `objects.png` sheet used is gone with it. See
  [BoothWindows](booth-windows.md) for the frame's authoring.
- Agents and Markers, keeping the existing icon-fit dimensions.
- Lift car: the sprite fills the car's own extent and follows the car's live
  shape through the shaft.
- Platform lift car: the sprite follows the moving slab's actual bounds, not the
  shaft bounds. The editor shaft outline is retained.
- Shuttle carriage: composed from the `shuttle-L0`/`-M0`/`-R0` role tiles at any
  width, with the middle reused, and the `shuttle-connector` drawn at the seam
  between carriages.

Bulkhead Doors, Access panels, ordinary Doors, Windows, Booth Windows, Buttons,
Markers and the three vehicles render from `ObjectAtlas` when that set is
installed, and keep their flat fills against a set that has no tile for them. Force
Bridges remain entirely primitive-rendered - see below. Other
transport geometry and editor overlays are unchanged. Headless tests without a
texture retain the primitive/font fallback.

The shared renderer loads and validates both the definition and PNG beside the
executable, under `textures/`. CMake deploys them even on builds without C++
changes. Missing or invalid assets cause a controlled startup error, and the GL
texture is released before the context is destroyed.

The original generated art is in `resources/textures/source/objects-generated.png`.
Run `python3 scripts/pack_object_atlas.py` (requires Pillow) to reproduce the packed
PNG and YAML. This crops the irregular generated sheet, normalizes cells, removes
near-transparent noise and neutralizes tintable sprites. The generated art remains
a draft; the packer makes its sizing and alpha layout deterministic.

The `sector-tileset` CTest also checks object quads, inverted screen-Y bounds,
Agent tint, door UV cropping, fully-open leaf suppression and cleanup.
The `render` smoke module's `objectAtlas/*` checks prove the loader contract
itself: an invented atlas on a 32x40 authored grid loads, resolves its regions
in that grid's units and draws them, while a content rectangle escaping its
cell or a grid leaving a remainder in the image is refused on the stated
requirement.
