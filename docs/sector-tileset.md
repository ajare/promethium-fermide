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

Counting rows up from the floor line is what keeps the anchor out of reach of
the viewport, and it makes the first row worth emitting a subtraction. That
subtraction rounded up until #1048, which rounded past the row straddling the
clip's bottom edge: scroll a Sector until its floor line sits below the
bottom of the viewport and its lowest visible row - 319 of its 320 pixels on
screen - drew nothing at all. Truncation is the fix, the shape the columns
beside it already used. The row the cut lands in is emitted at its full World
cell sampling its whole tile, and the scissor crops it at the clip, so the
band that reaches the screen is still one artwork pixel to one screen pixel.
A clip bottom landing exactly on a row boundary is unchanged: the row whose
bottom edge *is* the cut lies under the clip, not above it. `smoke/render/SectorSurfaceBottomClip.cpp`
pins all of that, and `smoke/render/SectorSurfaceVariants.cpp` pins the part
an author actually notices - a variant painted on that row draws rather than
vanishing.

## SectorAtlas

`resources/textures/sectors-new.tileset.yaml` is the version 2 authored tileset
behind `SectorAtlas`: ten full-cell Infrastructure tiles on the 128x320 grid
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
- **A `-Vn` name authored beside its own family.** That name is the derivation's
  to give. A hand-authored `corridor-V1` would collide with the derived tile, and
  which of the two won would come down to map order.

The same rule and the same refusals govern the object-shaped atlases, which took
the capability in #1053. They are not a second implementation of it: the
naming, the placement and the refusals all live in the shared authored-grid
loader, and the sector loader and the object loader each just say whether the
format they are reading derives families. See *Variant families* in
`docs/object-tileset.md` for the object side.

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
unchanged and the entries follow the Sector for free. A resize is the other
case, and the local key is re-derived there so the artwork stays on the World
cell it was painted on - see the next section.

The entries live on the record as well as on the live Sector for the same
reason in the other direction: a fact held only on the live object would
evaporate on the first resize, and a fact held only on the record leaves the
renderer walking the record list to answer a question it asks once per cell.

### What a resize and a move do to the entries

A resize does not move artwork. It either removes it or creates new tiles
(#1047). A cell that existed before the resize and still exists after keeps
whatever it was painted with, on the same World cell. Cells the edit deletes
take their artwork with them. Cells the edit adds arrive as base. Nothing is
ever shifted sideways.

The entries are stored Sector-local, keyed against the Sector's own
**bottom-left**, so an edit that moves that anchor silently re-points every
surviving entry at a different World cell unless the key moves with it. The
shared helper therefore re-keys through World coordinates on every resize:

```
worldColumn = oldAnchorX + entry.column
worldLevel  = oldAnchorY + entry.level
newColumn   = worldColumn - newAnchorX
newLevel    = worldLevel  - newAnchorY

drop when newColumn < 0 || newLevel < 0
      || newColumn >= cellsWide || newLevel >= levelsHigh
```

The arithmetic is signed on purpose. `column` and `level` are `uint32_t`, and
an entry whose World cell the edit just deleted shifts by a negative amount;
left unsigned that wraps to a huge value the extent check happens to catch on
one axis and silently misplaces on the other.

A **move is not a resize**. A move translates the whole Sector and the
artwork rides along with it, which in the stored local keys means unchanged.
The helper recognises this itself - an edit that leaves the footprint the same
size it was returns zero dropped and touches nothing - rather than leaving
every call site to remember, because a move asked as a World-cell re-key would
drop every entry the moment it left the old footprint, which is the opposite
of what picking the Sector up means.

An entry can only be lost by losing its cell, and an entry naming a cell the
new footprint does not have is **dropped**, not clamped inward: sliding it
onto the nearest surviving cell would put artwork where nobody painted it and
leave the author to hunt down every cell that had been moved under them. The
record rewrite drops them before the replay, which is what keeps the loader's
out-of-grid refusal from turning a resize into a failed edit. The plan reports
the loss - `Drop 2 variant cells outside the new footprint` - so the author
confirms it rather than discovering it, and a Room shrunk back to no entries
saves with no field at all, the same as one that never wore a variant. The
announced count is computed through the same helper that performs the re-key,
on a copy, so the line shown cannot disagree with the entries that go.

That includes the case that follows from the rule rather than being carved out
of it: a resize whose new footprint overlaps the old one nowhere leaves none
of the old cells behind, so it leaves none of the artwork behind, and the plan
says every entry went rather than letting a total loss look like a clean edit.

The four vertical Transits are carried through the same rule and the same
helper, not a copy of it written four times (#1045). What differs is which
edges of the footprint each type is allowed to move, and therefore which
entries it can ever lose:

| Transit | Cells wide | Levels high | What a resize can cost |
| --- | --- | --- | --- |
| Lift | 1 or 2 | 2 or more | either end of either axis - a shortened shaft and a narrowed one in the same edit |
| Ladder | 1, fixed | 2 or more | the ceiling and the floor line |
| Stairwell | 2, fixed | 2 or more | the ceiling and the floor line |
| Staircase | 2 or more | 2, fixed | either end of the flight |

So `above the new ceiling` and `below the new floor line` are the Lift's, the
Ladder's and the Stairwell's losses and `outside the new width at either
end` is the Lift's and the Staircase's, and all of them are the same question
asked of the same grid. A move costs any of them nothing, for the same reason
a Room move costs nothing: the artwork rides along with the Sector. Note that
a Lift's and a Staircase's own `move` flag reports any position change, even
one made together with a size change; the re-key keys off the footprint's size
rather than off that flag, which is why a Lift moved and shortened in one edit
is re-keyed and a Lift merely moved is not.

The four horizontal Transits go through the same rule and the same helper
(#1046), and the same question - which edges of the footprint can this type
move - decides what each can lose:

| Transit | Cells wide | Levels high | What an edit can cost |
| --- | --- | --- | --- |
| Shuttle | 1 or more | 1, fixed | either end of the track |
| Airlock | 1 or more | 1, fixed | every cell it stood on - a width change is a relocation |
| Chamber | 1 or more | 1, fixed | every cell it stood on - a width change is a relocation |
| Dumbwaiter | 1, fixed | 2, fixed | nothing - it is never resized |

A Shuttle is one level high however long it runs, so the track's two ends are
the only edges it can move, and shortening it at either end costs the entries
beyond the new end while the survivors stay on the World cells they were
painted on. An Airlock's or Chamber's width is not free: both of its ends
must adjoin a Room or Corridor end, so the width it can hold is the gap its
row happens to have, and a different width means a different row. A one-level
Sector moved to another row shares no cell with where it stood, so every
chamber width change - narrowing or widening - is the no-overlap case, and
all of its artwork goes. That is the rule arriving at its end rather than a
fixture accident, and it is stated here because the chamber plan has no
confirmation step to say it through. A Dumbwaiter is a fixed 1x2 shaft that
is moved rather than resized at all, which is where the Sector-local keys
earn their keep most visibly: the entries ride across unchanged with no
re-keying, because there is no re-keying to get wrong. The move replaces the
Sector object whole, so carrying the entries is what the move has to do rather
than something it gets to keep by not touching them.

One difference is worth stating rather than papering over. The Shuttle plan
confirms and so announces its drop in the same line the Location and vertical
Transit plans use; the chamber plans commit without a confirmation step and
have no consequences to announce, so a width change costs its artwork without
being asked about it.

What the loader refuses:

- **An entry outside the Sector's own grid.** A column past `cellsWide` or a
  level past `levelsHigh` names a cell that does not exist. Refusing it is a
  loud miscount; drawing nothing for it is a silent one.
- **Two entries for one cell.** Which variant the cell wears would come down
  to list order, so the loader refuses instead of choosing.
- **`variant: 0`.** The base is the absence of an entry, not a zero stored
  in one. A recorded zero is a third state that means the base without being
  it: the artwork has no `-V0` row so the renderer falls back to base and
  the picture looks right, while the Sector answers `0` rather than `nullopt`
  to every "is there a variant on this cell?" and the save keeps writing a
  field for a Sector that never wore one. The authoring seam has always
  refused it; the load seam refuses it too, so the two agree on what a legal
  entry is (#1049). Refused rather than silently dropped, for the same reason
  as the two rules above - a repair would read as a clean load and hide the
  miscount.
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

The variant is derived from the family the Sector belongs to, never from the
name the draw ended up with (#1050). Two questions meet at the surface seam
and only one of them owns the variant. Which family a Sector is in is
`sectorSurfaceKind`'s answer, and no artwork installed changes it: an
Airlock next door to a Corridor is still an Airlock. Which tile it draws is
the borrow rule's answer, and is entirely a fact about the sheet - an
Airlock with no `airlock` tile wears the Corridor's base, a Dumbwaiter the
Lift's. Deriving the variant from the drawn name agrees with both for every
type that owns its tile, which is every type the shipped set declares, and
leaks the moment a Sector borrows: on a sheet carrying `corridor-V2` and no
`airlock` tile, an Airlock recorded as wearing variant 2 asked for
`corridor-V2`, found it, and drew a Corridor's variant - neither the tile
the document recorded nor the base the fallback answers with. Asking the
own family makes that unrepresentable: a borrowing Sector has no
own-family variant tile to find, so its variant cells fall back to the
borrowed base their plain neighbours are already drawing. The borrow rule
keeps deciding the base and nothing else.

The shape only shows on a partially-authored sheet - one type's variants
present while another type's base is absent - which is what an author has
under way far more often than the shipped `SectorAtlas` with all ten
types and three variants each, and is exactly when a wrong tile reads as
unfinished art rather than as a bug.

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
it is what the #1013 spec's dimensions table records for `SectorAtlas`
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

The `ImageSet` entries the application manifest declares for `SectorAtlas` are
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

## The doorframe overlays

A cell a Door stands in does not draw its wall's tile. It draws that wall's
**doorframe** tile: the moulding around the opening, with the leaf
animating inside it and the space behind showing through the hole. The
frame is part of the wall rather than part of the Door, so it is keyed by
the wall and lives in this atlas, while the leaf is keyed by the Door's
function and lives in the Objects set (#1052).

They are authored in a sibling `overlays` map rather than among the
`sprites`:

```yaml
overlays:
  doorframe-room-1r-L0:
    cell: [10, 0]
    content: [0, 0, 128, 320]
    offset: [0.0, 0.0]
    drawn_size: [1.0, 1.0]
    tintable: true
    hole: [13, 160, 102, 160]
```

The name is `doorframe-<wall>-<width><height>-<role>`: the wall is `room`
or `corridor`, the width is 1 or 2 cells, the height is `r`egular or
`t`all, and the role is `L0` for the opening's left cell and `R0` for its
right. A 1-wide opening has only an `L0`. Corridors get no tall frame,
because a tall Door is refused in a Corridor and a tile nothing can ever
reach is artwork that reads as unfinished. Nine tiles in all, in the three
columns to the right of the surface strip, which is why the sheet is 1664
x 1280 rather than the 1280 x 1280 it was.

The second map is not tidiness. `hasSectorTileSurface()` is asked "is this
a Sector surface" by the borrow rule and by the editor's swatch strip, and
neither of them wants a doorframe coming back from that question. A frame
is a tile of the atlas and not a surface a Sector is authored with, so it
answers `hasSectorTileOverlay()` instead. The per-cell surface loop can
still draw one: a resolved name that is not a surface is looked for among
the overlays before the cell falls back to the missing-art colour, which
is what lets the frame arrive through the loop #1037 pinned instead of
through a second loop that would have to relearn the anchor, the crop and
the culling.

What the loader refuses:

- **A name authored in both maps.** Which kind the draw found it in would
  come down to lookup order, and a frame drawing as a wall - or a wall
  drawing as a frame - is not a thing two maps should be left to settle.
  This catches an overlay wearing a derived variant's name too, since the
  derived tiles are already in the surface map by then.
- **`variants` on an overlay.** A surface's variants are different
  pictures of the same thing, picked from a swatch strip. A frame is not
  picked at all - it is derived from the Door standing in the wall - so a
  family of frames is a choice nobody can make. Refused rather than
  ignored, the same reason the version 1 formats refuse the key.
- **The `boundary-` prefix on an overlay.** The install routes that prefix
  to the boundary map before it looks at anything else, so an overlay
  wearing it would arrive as edge artwork nobody asked for.
- **A `hole` outside its own cell or outside the content it was authored
  inside.** A hole that ran past the cell would punch a neighbour's
  artwork.

### The hole, rounded down

Every frame declares a `hole`: the transparent opening it frames, in image
pixels from its own cell's top-left. The generator punches it, so the
placeholder sheet shows the frame's shape rather than a solid slab, and
what the tileset states is what the artwork has.

The hole is rounded **down** from the aperture it frames - inward on every
edge. A 0.8-wide aperture on a 128-pixel cell is 102.4 pixels; the hole
is 102, inset from 13 to 115. The leaf is clipped to the aperture, so a
hole smaller than the aperture costs at most a fraction of a pixel of
moulding covered by the leaf. A hole larger than the aperture leaves a
ring of whatever lies behind the wall showing outside the leaf at every
open percentage, which is unambiguously wrong. The hole reaches the cell's
bottom edge because the aperture does: there is no sill in this geometry.

`scripts/tests/doorframe_tiles.py` pins the rounding against the aperture
each frame frames, checks the committed sheet is actually punched where
the tileset says, and checks the frames are declared in the `ImageSet` the
install pairs against.

### What a door cell resolves to

The resolver #1039 left open is where the frame arrives, and it answers
from the live Door objects rather than from anything recorded:

- a cell of a Room or a Corridor that a Door covers, with that Door's
  **front** side on this Sector, answers the frame for that Door's width
  and height and the cell's role in the opening;
- every other cell answers the variant it was recorded with, or the base,
  exactly as before;
- a frame the installed atlas does not carry is no frame at all, and the
  cell falls back to its wall's tile rather than to the missing-art
  colour. That is what makes the feature additive: a World drawn on the
  version 1 family, or on any set with no frames authored, keeps drawing
  what it drew before.

Because the answer is computed from what is standing in the wall, nothing
is consumed. A variant an author painted on a cell the Door later covers
is outranked for as long as the Door is there and draws again when the
Door is taken out - the entry underneath was never touched. A Door is a
reversible placement, and so is taking it back.

The frame follows the wall's own family, not the surface the borrow rule
settled on, for the same reason #1050 keeps a borrowing Sector off the
borrowed family's variants: an Airlock beside a Corridor is still an
Airlock, and has no frame of its own authored.

Only the front side of a threshold is framed. A Door is registered on both
Sectors of the pair it joins, and the Sector behind is seen *through* the
opening rather than framing it, so it keeps its own base tile. Rendering
the back face of a threshold is out of the feature's way.

`smoke/render/DoorframeCells.cpp` pins all of it: the 1-wide and 2-wide
regular frames in a Room, the tall pair for a tall Door, the same three
cases in a Corridor, the frame taking its row's floor-anchored crop and
the same culling as the plain cell beside it, the painted variant hidden
and coming back, two adjacent doors each framing their own cells without
leaking, a removed Door reverting with no residue, the back wall taking no
frame, and the fallback when no frame is authored.

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

Since the #1013 terminus `SectorAtlas` is the only Sector atlas a World defaults
to, and the old sheet it displaced was deleted (#1024). `StationAtlas` stays
registered as a custom theme a World may point its Sector slot at; it is not a
family, and nothing borrows across the two.

## Generation and installation

The grid resolution the version 2 sector and object tilesets share lives in
`pf-render/src/AuthoredTileGrid.cpp`, so a cell-and-content sheet is read the same
way whichever atlas it fills. Pixel regions still come from the `ImageSet` and
render semantics still come from the authored tileset - the split ADR 0010 as
amended describes.

Whether a base sprite may declare a variant family is a parameter of that shared
loader and not a fact about grids, because it is not the grid that has to keep the
promise. The sector loader passes `Derived`; the object loader passes `Derived`
for a version 2 set and `Refused` for a version 1 one, which is the default and
refuses a sprite carrying the key. The derived tiles are named and placed by the
shared grid too, since #1053 - the `-V` rule is arithmetic over the base's cell
and the declared count, not vocabulary one loader owns and the other copies - and
the grid is what establishes that the declared count has room to stand on the
declared sheet. Each loader keeps the count with the base that stated it, so a
caller can ask what was declared rather than infer it from what happened to be
present.

CMake copies both assets beside each executable under `textures/`, including
on builds with no C++ changes. Startup resolves that directory relative to the
executable, not the working directory. Invalid or missing assets produce a
controlled startup error. The GL texture is deleted before the GL context.
Headless rendering without an installed texture retains the primitive fallback.

Run `ctest --test-dir build-linux -R 'sector-tileset|smoke-startup|smoke-render' --output-on-failure`
after building the GUI, headless and `pf-sector-tileset-checks` targets.
