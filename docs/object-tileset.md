# Object atlas

`resources/textures/objects.png` is a 320x960 RGBA atlas laid out on a uniform
64x160 authored grid: 5 cells across by 6 down, one world unit to a cell rather than
a 64x64 square of screen pixels. The World cell's screen size is a separate fact:
the renderer stretches each resolved region into the World-unit rectangle it is
drawn into, so the artwork magnifies with the cell instead of being re-cut (#1007).
Unoccupied cell space is transparent. Small objects do not expand to fill their
cell.

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

## ObjectAtlasNew

`resources/textures/objects-new.tileset.yaml` is the version 2 authored tileset
behind the replacement Objects atlas, laid out on the 128x320 World cell at
768x960. The Walkway, the Bulkhead Door and the Access panel are live in it,
and the rest of the sheet drops in against the same image size. The old
`ObjectAtlas` and `ObjectAtlasImage` stay registered and canonical until
nothing references them.

The `ImageSet` entries the application manifest declares for `ObjectAtlasNew` are
generated from the tileset, not authored beside it:

```sh
python3 scripts/generate_object_atlas_new.py --write   # regenerate
python3 scripts/generate_object_atlas_new.py --check   # verify, as CI does
```

The render smoke module checks the committed manifest against the tileset on every
run, so a drift fails the build rather than showing up as artwork in the wrong
place.

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

### The Force Bridge is given no tile, deliberately

A Force Bridge's span is continuous and arbitrary - any fraction of a cell
over any gap - so a fixed tile would be stretched wrong at most lengths it is
used at, and the 2px line is honest about being an energy field. A version 2
object tileset that declares a `force-bridge` tile is refused by the loader, so
the decision cannot be reversed by an edit to a sheet nobody thought about.

## FurnitureAtlasNew and the tile naming convention

`resources/textures/furniture-new.tileset.yaml` is the version 2 authored tileset
behind `FurnitureAtlasNew`, the replacement Furniture atlas: ten full-cell tiles
on the 128x320 grid at 640x640 - `chair-L0`, and `sofa`/`desk`/`bed` each at
`-L0`, `-M0` and `-R0`. Every one of them is drawn at one full World cell with no
render offset, so a piece one cell wide covers one cell and a piece four cells wide
covers four. `scripts/generate_furniture_atlas_new.py` generates its sheet and its
manifest entries the same way the Objects atlas's does; both scripts share
`scripts/atlas_new.py`.

A Furniture tile name is `<piece>-<role><level>`. The role is `L`, `M` or `R` -
the left cap, a middle cell, the right cap - and the level is the height level
within the piece's footprint. The anchor is the **bottom-left** of that footprint,
so every offset is non-negative and no sign has to be encoded in a name, and one
digit per axis caps a piece at 9x9. The other vocabulary is `-XY`, an absolute
offset from the anchor, retained as the escape hatch for the irregular footprint a
role vocabulary cannot name.

The invariant the loader enforces is that `L` is always present because it is the
anchor, `R` appears when the piece is at least two cells wide, and `M` appears when
it is at least three. For height > 1 every role present must be present at every
height level, so a footprint can never be half-specified. A piece speaks one
vocabulary throughout: a footprint mixing role-suffixed and absolute-suffixed
tiles is refused, and so is a bare name inside a footprint that names its other
tiles by position.

The suffix is applied only where the piece can span more than one cell. Furniture
is that kind of thing, so all ten of its tiles carry a role suffix and a width-1
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

Names that carry no recognized suffix - `chair`, `sofa-left`, `button-enabled` -
are the old artwork's authoring and load exactly as they did. The convention binds
a piece that speaks it, not every catalogue written before it, which is what lets
the old `FurnitureAtlas` stay registered and canonical while the new set is
brought up beside it.

The `render` smoke module's `furnitureAtlasNew/*` checks prove all of this: the
ten tiles registered at full-cell drawn size, a three-cell sofa rendering from
`sofa-L0`, `sofa-M0` and `sofa-R0`, a four-cell sofa reusing the same middle
tile across two cells and naming three tiles in all, and each refusal at the
loader seam.

## AgentAtlasNew and the appearance-keyed grid

`resources/textures/agents-new.tileset.yaml` is the version 2 authored tileset
behind `AgentAtlasNew`: four Agent appearances, each carrying all five Poses,
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
  geometry. The transparent Open frame stays stationary; the opaque 42x38 shutter
  subregion of the Closed tile translates upwards behind it, clipped to its centre.
  See [BoothWindows](booth-windows.md) for authoring and regeneration.
- Agents and Markers, keeping the existing icon-fit dimensions.
- Platform lift car: the sprite follows the moving slab's actual bounds, not the
  shaft bounds. The editor shaft outline is retained.

Bulkhead Doors and Access panels render from `ObjectAtlasNew` when that set is
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
