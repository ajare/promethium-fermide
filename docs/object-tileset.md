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
768x1280. The Walkway, the Bulkhead Door, the Access panel, the Door, the three
Window styles, the Booth Window's frame, shutter and closed panel, the two Button
states and the Marker are live in it, along with the three vehicles - the Lift
car, the Platform lift and the Shuttle carriage - and the rest of the sheet
drops in against the same image size. The old `ObjectAtlas` and
`ObjectAtlasImage` stay registered and canonical until nothing references them.

The vehicles are what took the sheet from three rows to four. Nineteen tiles do
not fit an 18-cell grid, and cramming two into one cell would break the one
promise the grid makes to whoever draws the real art: that a tile owns its whole
128 x 320 slot.

The `ImageSet` entries the application manifest declares for `ObjectAtlasNew` are
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

The declared drawn size is the regular one-cell Door's own extent: `cellsWide -
2 * CORE_DOOR_X_INSET` across a regular Door's `CORE_DOOR_HEIGHT`, which is 0.8 x
0.5 World units. As with the Access panel, the object's own geometry is what the
artwork is drawn into; the declared size is the reference the art is authored
against, not a cap on what the object may be.

### The Booth Window is a fixed frame with a shutter behind it

A Booth Window composes three tiles rather than switching between three states of
one. `booth-window-open` is the frame - opaque at its edges, transparent through
its centre - and it never moves. The shutter is the opaque panel that slides
behind that frame, and the frame's transparent centre is what makes the slide
visible. `booth-window-closed` fills the whole aperture for the artwork path that
wants one tile for the closed state.

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
one `FurnitureAtlasNew` speaks further down; a carriage cell is one cell wide at
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

### The shipped catalogues were repointed (#1029)

Registering the role tiles was only half the migration. Every shipped catalogue
under `resources/test-worlds/` still named the pre-convention sheet - `chair`,
`sofa-left`, `desk-right` - and `FurnitureAtlasNew` carries none of those names,
so any World pairing the atlas a new World now defaults to with a shipped
catalogue drew every Furniture piece as the missing-tile magenta. The shipped
catalogues now speak the convention: `chair-L0`, `sofa-L0`/`sofa-R0`,
`bed-L0`/`bed-R0`, `desk-L0`/`desk-R0`, and the four-cell composition piece
reuses `sofa-M0` twice rather than needing a fourth tile.

Two things had to come with that repoint.

**The old sheets keep their Worlds.** A World that has not moved to
`FurnitureAtlasNew` - which is every document written before the selection
existed, plus the station theme - draws the old sheet, and the repoint must not
change a pixel of it. `FurnitureAtlas` and `StationFurnitureAtlas` therefore
carry each role name the shipped catalogues now use over the identical pixel
region as the old name it stands for. The alias says nothing new about the art:
`sofa-left` *is* the sofa's left cap and `chair` *is* the chair's anchor cell.
The middle roles are deliberately absent, because the old sheets never had
middle art; a piece wider than two cells was not something they could express.

**A footprint the sheet cannot serve is a stated gap.** `layouts.furniture.lua`'s
`larger` piece reaches a negative offset from its anchor and a second height
level with no left cap there, so neither the role vocabulary nor the `-XY`
escape hatch can name it as placed, and the sheet is a full 5x2 grid of
full-cell role tiles with no irregular art to give it. `uncovered_footprints`
at the top of `furniture-new.tileset.yaml` records it with the reason, in the
same shape as the Agent sheet's `reserved_appearances`. A record with no reason
is refused, as is one naming a piece the sheet now covers: a gap that is written
down is a decision, and one that is not is a magenta rectangle nobody noticed.

The `render` smoke module's `furnitureAtlasNew/*` checks prove all of this: the
ten tiles registered at full-cell drawn size, a three-cell sofa rendering from
`sofa-L0`, `sofa-M0` and `sofa-R0`, a four-cell sofa reusing the same middle
tile across two cells and naming three tiles in all, and each refusal at the
loader seam. The four added with #1029 read the shipped bundle itself rather
than a fixture made to conform - which is the whole point, since a synthetic
catalogue cannot see that the real one names something else:

- `everyShippedFurnitureCatalogueResolvesInEveryNewFurnitureAtlas` pairs every
  tile of every shipped catalogue with every `-New` Furniture atlas the atlas-set
  registry names, and a disagreement names the catalogue, the piece and the tile.
- `noShippedFurnitureRendersMagentaUnderTheNewDefaultAtlas` renders each shipped
  piece on its own under `FurnitureAtlasNew` and requires its artwork, then pulls
  the atlas out from under the same piece to prove the magenta detector is not
  deaf.
- `everyShippedFootprintIsCoveredOrRecorded` holds the covered-or-recorded rule
  and refuses a gap record that is reasonless or has stopped describing a gap.
- `theOldFurnitureAtlasesStillDrawTheirOwnArt` installs the old sheets the way
  the render stack does and requires each role alias to cover the identical
  region to the name it stands for, so an existing World's pixels are unchanged.

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

- every shipped Agent type's declared Pose tiles resolve in every `-New` Agent
  atlas the atlas-set registry names, and a failure names the type and the tile;
- every appearance row the sheet authors is named by at least one shipped Agent
  type, or is listed under `reserved_appearances` in the tileset with the reason it
  is held back. The generic `agent` row is reserved on that basis - it is the
  fallback a type with no artwork of its own names, and no shipped type needs it.
  A reservation with no reason is refused, and so is a reservation naming a row the
  sheet no longer authors, because a stale reservation is where the next orphan
  hides.

The shipped types are discovered from the bundled `.agent.lua` resources rather than
listed by hand, so a type added to the bundle is paired against the atlas the day
it lands.

The old `AgentAtlas` keeps its `robot-standing` tile. This ticket changes nothing in
the old sheet: it stays registered and canonical for every World that names it, and
a custom Agent type written against it still names that tile. A check pins the tile
resolvable there so it cannot be swept up as unused art the bundle no longer
references (#1028).

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
`ObjectAtlasNew` (#1026). The game has no artwork panel to show a refusal
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
  geometry. The transparent Open frame stays stationary; the opaque 42x38 shutter
  subregion of the Closed tile translates upwards behind it, clipped to its centre.
  See [BoothWindows](booth-windows.md) for authoring and regeneration.
- Agents and Markers, keeping the existing icon-fit dimensions.
- Lift car: the sprite fills the car's own extent and follows the car's live
  shape through the shaft.
- Platform lift car: the sprite follows the moving slab's actual bounds, not the
  shaft bounds. The editor shaft outline is retained.
- Shuttle carriage: composed from the `shuttle-L0`/`-M0`/`-R0` role tiles at any
  width, with the middle reused, and the `shuttle-connector` drawn at the seam
  between carriages.

Bulkhead Doors, Access panels, ordinary Doors, Windows, Booth Windows, Buttons,
Markers and the three vehicles render from `ObjectAtlasNew` when that set is
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
