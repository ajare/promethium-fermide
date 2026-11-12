#!/usr/bin/env python3
"""The windowframe tiles the shipped Sector atlas authors (#1075, #1076,
#1077, #1078).

A windowframe is the moulding laid over the wall's own tile around the
opening a Window stands in. Its transparent hole is the glass the frame
frames, and - like the doorframe's - it is the one part of the artwork
that cannot be wrong by a fraction of a pixel in the wrong direction.
The glass is drawn to the glass rect, so a hole authored *smaller* than
that rect costs at most a fraction of a pixel of moulding covered by
glass; a hole authored *larger* puts the moulding inside the pane, which
reads as a window smaller than the hole it was cut for and leaves a ring
of unglazed aperture showing outside the glass.

So the rule is: round the hole down, inward on every edge, from the glass
rect it frames. This checks that the authored hole is exactly that, that
the committed placeholder sheet really has the hole where the tileset
says, and that the frame is in the atlas's `overlays` map rather than
among its surfaces - which is what keeps `hasSectorTileSurface()`
answering "is this a Sector surface" and not "is this a tile".

The frame is keyed by where its cell sits in the opening's span and by
nothing else. The block on the sheet *is* the role grid: columns left to
right are the horizontal role `S L M R`, rows top to bottom are
single-level, top cell, middle cell, bottom cell, so an artist reads the
grid off the sheet rather than learning a numbering. The grid closes at
fifteen tiles - sixteen cells less the all-middle one, where no tile is
asked for - and those fifteen answer every cell of every span, so the
same tiles frame a 1x1 window and a 7x4 one.

The wide row is where the horizontal roles earn their keep. A span's inset
belongs to the *ends of the span*, not to every cell: a 2-wide Window's
glass runs from 0.1 of a cell in at one end to 0.1 in at the other, so
its left cell carries the glass from its own inset to its own right edge
and its right cell carries it from its own left edge to the inset. The
two holes meet at the seam between the cells instead of leaving a strip of
moulding across it, which is what makes a wide window read as one opening
rather than as two adjacent ones. The middle cell of a 3-wide span has no
inset at all, so its hole is the full width of its cell and there is no
vertical moulding in the middle.

The tall column says the same thing up the other axis, and it is checked
the same two ways - tile by tile, and then joined. A Window's glass stands
0.2 of a level above the *span's* floor line, is 0.3 of a level tall on
its first level and a whole level taller for every level past it, so its
top sits 0.5 of a level below the span's top however tall the span gets.
The bottom cell carries the glass from its own 0.2 up to its own top
edge; the top cell carries it from its own bottom edge up to 0.5 above
that cell's own floor line - the same image row the single-level `S` row
holes at, the two roles differing only at the bottom; every cell between
carries the whole cell height. So the bottom cell's hole stops above the
floor line exactly where the glass does - that 0.2 is the sill - and the
top cell's hole reaches the cell's bottom edge where the glass runs down
into it, and the holes join into one continuous aperture across the cell
boundary. A `BS` hole that ran down to the floor would cut the sill out of
the wall; a `TS` hole that stopped short of the cell's bottom edge would
lay a transom of moulding across the glass; an `MS` hole that missed any
of the cell would put a head or a sill in the middle of a span that has
neither.

The corners are both axes at once, and they are where the grid closes. A
cell that is an end of the span in both directions carries its jamb and
its head or sill together, joined rather than butted: `BL` is `B`'s sill
and `L`'s jamb on one tile, with the hole inset 0.1 at the left and
stopped 0.2 above the floor line and running to the seam on its other two
sides. Nothing is doubled because both mouldings are present, and nothing
is missing on the axis the tile shares with its edge-role neighbours, so
the corner's hole lines up with the `BM` beside it and the `ML` above it
and the joined aperture of a 2x2 or 3x3 span is one rectangle with no
step at the corner. That is checked here as the joined rect of the whole
span, because a corner that stopped short of the seam and one that
overshot it are both wrong in a way no single tile shows.

The Furniture role vocabulary cannot carry this shape, and the test keeps
it from being tried anyway. That convention is `<piece>-<role><level>`,
its trailing digit the height level within the footprint. A windowframe's
vertical role is end-versus-middle rather than a level - the bottom cell
of a one-level window and the bottom cell of a three-level window are both
level 0 and want different art - and `L`/`M`/`R` has no spelling for the
one-column cell that is inset on both of its edges. A name that tried to
read `windowframe-L0` as "left cell, level 0" would be a second meaning
for one convention in a second file, so the digit-suffixed shape is
refused here.

The wall cannot come back into the name either. The doorframe already
refused `doorframe-room-*`, `doorframe-corridor-*` and `doorframe-facade-*`
because a frame laid over a wall's own tile has nothing left to say about
which wall it lies on, and the same refusal is made here under the window
name so the wall-keyed shape cannot return wearing the new prefix.
"""

from __future__ import annotations

import math
import pathlib
import re
import sys

import yaml
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import atlas_new  # noqa: E402
from generate_sector_atlas_new import SPEC as SECTOR  # noqa: E402

CELL_WIDTH = 128
CELL_HEIGHT = 320

# The role grid, as the block is laid out on the sheet. Columns left to
# right are the horizontal role; rows top to bottom are the vertical
# role. The bottom-of-window tiles carry the highest y and sit lowest on
# the sheet, the way they sit lowest in the window.
HORIZONTAL_ROLES = ["S", "L", "M", "R"]
VERTICAL_ROLES = ["S", "T", "M", "B"]

# The first column of the windowframe block. The twelve surface columns
# come first, then the three doorframe columns, then the four fresh ones
# the windowframe block stands in.
BLOCK_FIRST_COLUMN = 15

# The frames this set authors, with the glass rect each one frames in
# World units: the horizontal span across its cell, and the span's bottom
# and top measured up from its cell's floor line. A Window is inset 0.1 at
# each end of its *span*, stands 0.2 above the span's floor line and tops
# out 0.5 of a level below the span's own top, and every one of those is
# constant per role however big the span gets - which is what lets a fixed
# tile cover the opening whatever the opening's total size turns out to be.
# The horizontal span below is therefore the part of the span that falls
# inside the cell the tile frames: inset at the span's ends, and running
# edge to edge wherever the cell is not an end. The vertical span is the
# same read up the other axis: inset at the span's ends, whole wherever
# the cell is between them.
GLASS_RECTS = {
    # The 1x1 Window: inset on both of its edges, because both are ends.
    "windowframe-SS": (0.1, 0.9, 0.2, 0.5),
    # The left cell of a wide span: inset at the span's left end, and the
    # span carries on past the cell's right edge, so the glass does too.
    "windowframe-SL": (0.1, 1.0, 0.2, 0.5),
    # The middle cell of a 3-wide span: no inset anywhere inside it, so
    # the glass runs the full cell width and no vertical moulding does.
    "windowframe-SM": (0.0, 1.0, 0.2, 0.5),
    # The right cell of a wide span: the mirror of the left.
    "windowframe-SR": (0.0, 0.9, 0.2, 0.5),
    # The top cell of a tall single-column span: the span's glass runs
    # down past this cell's bottom edge, so nothing is inset at the seam
    # below, and it stops at the glass top - 0.5 of a level above this
    # cell's own floor line, which is `levelsHigh - 0.5` above the span's.
    # The head moulding lies over the glass above that.
    "windowframe-TS": (0.1, 0.9, 0.0, 0.5),
    # A middle level of a tall single-column span: the glass runs straight
    # through, inset nowhere inside the cell, so the hole is the whole cell
    # height and no head or sill crosses the middle of the window.
    "windowframe-MS": (0.1, 0.9, 0.0, 1.0),
    # The bottom cell of a tall single-column span: the glass stands 0.2
    # of a level above the span's floor line, and that 0.2 is the sill, so
    # the hole stops there and carries on up to this cell's own top edge.
    "windowframe-BS": (0.1, 0.9, 0.2, 1.0),
    # The corners and the interior edges: the same two insets, read on both
    # axes at once. A corner is an end of the span in both directions, so
    # it carries both mouldings; an interior edge is an end in one and
    # middle in the other, so it carries one.
    #
    # Top-left: the span's left inset and the head, running to the seam on
    # the right and below.
    "windowframe-TL": (0.1, 1.0, 0.0, 0.5),
    # Top-middle: no horizontal inset at all inside a middle column, so
    # this is the head alone across the full cell width.
    "windowframe-TM": (0.0, 1.0, 0.0, 0.5),
    # Top-right: the mirror of the top-left.
    "windowframe-TR": (0.0, 0.9, 0.0, 0.5),
    # Middle-left: a level that is neither end of the span vertically, at
    # the span's left end - jamb only, the whole cell height.
    "windowframe-ML": (0.1, 1.0, 0.0, 1.0),
    # Middle-right: the mirror.
    "windowframe-MR": (0.0, 0.9, 0.0, 1.0),
    # Bottom-left: the span's left inset and the sill.
    "windowframe-BL": (0.1, 1.0, 0.2, 1.0),
    # Bottom-middle: the sill alone across the full cell width.
    "windowframe-BM": (0.0, 1.0, 0.2, 1.0),
    # Bottom-right: the mirror of the bottom-left.
    "windowframe-BR": (0.0, 0.9, 0.2, 1.0),
}

# The exact hole each frame authors, restated independently of the
# rounding helper below. The helper computes the same value from the
# glass rect; this is the number written into the tileset, written out
# here so a change to the helper that moved the hole would have to move
# this too, and the two would have to be moved in concert for the exact
# check to keep passing while the inequality half still pins the hole
# inside the aperture.
EXPECTED_HOLES = {
    "windowframe-SS": (13, 160, 102, 96),
    "windowframe-SL": (13, 160, 115, 96),
    "windowframe-SM": (0, 160, 128, 96),
    "windowframe-SR": (0, 160, 115, 96),
    "windowframe-TS": (13, 160, 102, 160),
    "windowframe-MS": (13, 0, 102, 320),
    "windowframe-BS": (13, 0, 102, 256),
    "windowframe-TL": (13, 160, 115, 160),
    "windowframe-TM": (0, 160, 128, 160),
    "windowframe-TR": (0, 160, 115, 160),
    "windowframe-ML": (13, 0, 115, 320),
    "windowframe-MR": (0, 0, 115, 320),
    "windowframe-BL": (13, 0, 115, 256),
    "windowframe-BM": (0, 0, 128, 256),
    "windowframe-BR": (0, 0, 115, 256),
}

# The grid closes at fifteen. Sixteen cells in the role grid less the
# all-middle one, which asks for no tile at all: the middle of a span
# that is middle in both directions is all glass with no moulding on it,
# so there is nothing to author there and nothing to draw. Fifteen tiles
# is the whole of the vocabulary - every cell of every span, from a 1x1 to
# a 7x4, is one of these fifteen positions, so no further tile is ever
# needed for a bigger opening.
GRID_CELLS = len(HORIZONTAL_ROLES) * len(VERTICAL_ROLES)
ALL_MIDDLE = "MM"
FRAME_COUNT = GRID_CELLS - 1

# The wall-keyed frames the doorframe already refused, restated under the
# window name. A frame laid over the wall's own tile does not vary with
# the wall, and the fifteen `doorframe-room-*`/`-corridor-*`/`-facade-*`
# tiles this replaced were three copies of artwork that could not tell the
# copies apart. The same argument applies here word for word, so the shape
# is refused under the new prefix as well rather than being left free to
# come back as though a window frame needed a wall in its name.
FORBIDDEN = [
    f"windowframe-{wall}-{vertical}{horizontal}"
    for wall in ("room", "corridor", "facade")
    for vertical in VERTICAL_ROLES
    for horizontal in HORIZONTAL_ROLES
]

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def load_tileset() -> dict:
    with (ROOT / SECTOR.tileset).open(encoding="utf-8") as stream:
        return yaml.safe_load(stream)


def expected_hole(rect: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    """The hole the glass rect allows, rounded down into whole pixels.

    Inward on every edge: the left edge rounds up, the right edge rounds
    down, and the height rounds down with the top edge held at the rect's
    top so the shortfall comes off the bottom of the pane rather than
    opening a gap at the head. Never a pixel outside the rect.
    """
    x0, x1, v0, v1 = rect
    left = -(-x0 * CELL_WIDTH // 1)          # ceil
    right = int(x1 * CELL_WIDTH)            # floor
    bottom = int(v0 * CELL_HEIGHT)          # floor
    top = int(v1 * CELL_HEIGHT)             # floor
    # Image rows run downward from the cell's top-left; the floor line is
    # the cell's bottom edge.
    return (left, CELL_HEIGHT - top, right - left, top - bottom)


def main() -> int:
    tileset = load_tileset()
    overlays = tileset.get("overlays")
    if not isinstance(overlays, dict):
        print("FAIL: the sector tileset has no `overlays` map for its "
              "windowframes")
        return 1

    authored = [name for name in overlays if name.startswith("windowframe-")]
    check(set(authored) == set(GLASS_RECTS),
          f"the authored windowframes are {sorted(authored)}, expected "
          f"{sorted(GLASS_RECTS)}")
    check(len(GLASS_RECTS) == FRAME_COUNT,
          f"this test states {len(GLASS_RECTS)} windowframes; the role grid "
          f"closes at {FRAME_COUNT} - {GRID_CELLS} cells less the all-middle "
          "one, which asks for no tile")

    # The wall-keyed shape is refused under the window name, the same way
    # the doorframe refuses it, so the fifteen copies that were dropped
    # cannot come back wearing the new prefix.
    for name in FORBIDDEN:
        check(name not in overlays,
              f"{name} is authored under a wall key; a frame laid over the "
              "wall's own tile does not vary with the wall, and the "
              "wall-keyed frames the doorframe already refused are refused "
              "here too")
    for name in authored:
        for wall in ("room", "corridor", "facade"):
            check(wall not in name,
                  f"{name} carries the wall '{wall}'; the frame is keyed by "
                  "its place in the span and by nothing else")

    # The sheet grew to the size the role grid needs: nineteen columns by
    # four rows, so the block has four columns of its own and four rows
    # to stand its four vertical roles in.
    width, height = tileset["size"]
    check(width == 19 * CELL_WIDTH and height == 4 * CELL_HEIGHT,
          f"the atlas sheet is {width}x{height}; the windowframe block "
          f"needs the sheet at {19 * CELL_WIDTH}x{4 * CELL_HEIGHT} - "
          f"nineteen columns by four rows of {CELL_WIDTH}x{CELL_HEIGHT}")

    for name in GLASS_RECTS:
        sprite = overlays.get(name)
        if sprite is None:
            continue

        # The frame is an overlay, not a surface: the map it is authored
        # in is what keeps `hasSectorTileSurface()` answering about
        # Sector surfaces alone, and keeps the editor's swatch strip from
        # ever offering a frame to paint.
        check(name not in tileset.get("sprites", {}),
              f"{name} is authored among the surfaces; a windowframe is "
              "not a Sector surface, and a frame is never painted")

        # It declares no variant family. A surface's variants are
        # different pictures of the same thing an author picks from a
        # swatch strip; a frame is not picked at all, it is derived from
        # the Window standing in the wall, so a family of frames is a
        # choice nobody can make.
        check("variants" not in sprite,
              f"{name} declares a variant family; a frame is derived from "
              "the opening it frames and has no family of pictures to pick "
              "from")

        # The block's place on the sheet is the role grid. The name says
        # the roles; the cell says the same thing positionally, and the
        # two have to agree or the sheet stops being readable.
        vertical, horizontal = name[len("windowframe-"):]
        check(vertical in VERTICAL_ROLES and horizontal in HORIZONTAL_ROLES,
              f"{name} is not named from the role grid: the vertical role "
              f"is one of {VERTICAL_ROLES} and the horizontal one of "
              f"{HORIZONTAL_ROLES}")
        if vertical not in VERTICAL_ROLES or horizontal not in HORIZONTAL_ROLES:
            continue
        check(f"{vertical}{horizontal}" != ALL_MIDDLE,
              f"{name} is the all-middle cell of the grid; a span that is "
              "middle in both directions is all glass with no moulding and "
              "asks for no tile at all")
        column, row = sprite["cell"]
        check(column == BLOCK_FIRST_COLUMN
                + HORIZONTAL_ROLES.index(horizontal)
                and row == VERTICAL_ROLES.index(vertical),
              f"{name} is in sheet cell [{column}, {row}]; the role grid "
              f"puts the '{vertical}' row at {VERTICAL_ROLES.index(vertical)} "
              f"and the '{horizontal}' column at "
              f"{BLOCK_FIRST_COLUMN + HORIZONTAL_ROLES.index(horizontal)}, "
              "because the block is the grid and the bottom-of-window "
              "tiles sit lowest on the sheet")

        hole = sprite.get("hole")
        check(isinstance(hole, list) and len(hole) == 4,
              f"{name} declares no `hole` rectangle")
        if not (isinstance(hole, list) and len(hole) == 4):
            continue

        # Stated once as the exact expected value...
        want = expected_hole(GLASS_RECTS[name])
        check(tuple(hole) == want,
              f"{name} authors hole {tuple(hole)}; the glass rect "
              f"u[{GLASS_RECTS[name][0]},{GLASS_RECTS[name][1]}] "
              f"v[{GLASS_RECTS[name][2]},{GLASS_RECTS[name][3]}] rounds "
              f"down to {want}")
        check(tuple(hole) == EXPECTED_HOLES[name],
              f"{name} authors hole {tuple(hole)}, which is not the value "
              f"this test states independently of the rounding helper "
              f"({EXPECTED_HOLES[name]}); the two are meant to be the same "
              "number arrived at two ways")

        # ...and again as an inequality against the aperture, so a change
        # to the rounding helper cannot make both halves of this agree by
        # moving together: the exact check pins where the hole is, and
        # this one pins that wherever it is, it is inside the pane.
        x, y, hole_width, hole_height = hole
        x0, x1, v0, v1 = GLASS_RECTS[name]
        hole_v0 = (CELL_HEIGHT - (y + hole_height)) / CELL_HEIGHT
        hole_v1 = (CELL_HEIGHT - y) / CELL_HEIGHT
        check(x / CELL_WIDTH >= x0 - 1e-9
                and (x + hole_width) / CELL_WIDTH <= x1 + 1e-9
                and hole_v0 >= v0 - 1e-9
                and hole_v1 <= v1 + 1e-9,
              f"{name}'s hole reaches outside the glass rect it frames")
        check(hole_width > 0 and hole_height > 0,
              f"{name}'s hole has no area, so the frame would be a solid "
              "slab over its own opening")

    def hole_of(name: str) -> tuple[int, int, int, int]:
        return tuple(overlays[name]["hole"])

    # The wide row frames *one* opening. The left cell's hole has to run
    # to that cell's own right edge and the right cell's hole has to start
    # at its own left edge, so laid side by side the two are one aperture
    # with no strip of moulding across the middle; and the middle cell of a
    # 3-wide span carries no vertical moulding at all. Checked as the
    # joined rect too, against the glass rect of the whole 2- and 3-cell
    # span, because a hole that stops short of the seam and a hole that
    # overshoots it are both wrong in a way no single tile shows.
    wide = [hole_of(n) for n in ("windowframe-SL", "windowframe-SM",
                                "windowframe-SR") if n in overlays]
    if len(wide) == 3:
        left, middle, right = wide
        check(left[0] + left[2] == CELL_WIDTH,
              "windowframe-SL's hole stops "
              f"{CELL_WIDTH - (left[0] + left[2])}px short of its cell's right "
              "edge, which puts a vertical moulding across the middle of a "
              "wide window and makes it read as two")
        check(right[0] == 0,
              "windowframe-SR's hole starts "
              f"{right[0]}px in from its cell's left edge, which puts a "
              "vertical moulding across the middle of a wide window")
        check(middle[0] == 0 and middle[0] + middle[2] == CELL_WIDTH,
              "windowframe-SM's hole does not span its whole cell, so the "
              "middle of a 3-wide window carries vertical moulding")
        # Same band top and bottom, or the joined aperture is not a rect.
        check(left[1] == middle[1] == right[1]
                and left[3] == middle[3] == right[3],
              "the wide row's holes are not the same vertical band, so the "
              "joined aperture is not rectangular across the span")
        # The joined holes, against the glass rect of the whole span: a
        # 2-cell span is inset 0.1 at each end, a 3-cell span likewise.
        for cells, parts in ((2, (left, right)), (3, (left, middle, right))):
            joined = sum(part[2] for part in parts)
            # The whole span's glass rect, u[0.1, cells - 0.1], rounded
            # down inward on both ends.
            want = int((cells - 0.1) * CELL_WIDTH) - math.ceil(0.1 * CELL_WIDTH)
            check(joined == want,
                  f"a {cells}-cell span's holes joined are {joined}px wide; "
                  f"the span's glass rect u[0.1,{cells - 0.1}] rounds down "
                  f"to {want}px")

    # The tall column frames *one* opening up the vertical axis exactly as
    # the wide row frames one across the horizontal. Each tile carries the
    # part of the span's glass that falls inside its own cell, so laid one
    # above the other the holes are a single aperture with no transom of
    # moulding lying across the middle of the window. Checked at the edges
    # of each tile and then as the joined rect of the whole 2- and 3-level
    # span, because a hole that stopped short of the seam and one that
    # overshot it are both wrong in a way no single tile shows.
    tall = [hole_of(n) for n in ("windowframe-BS", "windowframe-MS",
                                "windowframe-TS") if n in overlays]
    if len(tall) == 3:
        bottom, middle, top = tall
        # The bottom cell's hole stops above the cell's floor line where
        # the glass does - the 0.2 that is the sill - and reaches the
        # cell's own top edge, which is where the cell above takes over.
        check(bottom[1] == 0,
              "windowframe-BS's hole stops "
              f"{bottom[1]}px below its cell's top edge, which puts a "
              "transom of moulding across the join with the level above and "
              "breaks the aperture in two")
        check(bottom[1] + bottom[3] == CELL_HEIGHT - math.floor(0.2 * CELL_HEIGHT),
              "windowframe-BS's hole does not stop 0.2 of a cell above the "
              f"floor line: it ends at {bottom[1] + bottom[3]}px from the "
              f"cell top, and the sill is the {CELL_HEIGHT}px cell's bottom "
              "0.2, so the sill is either cut out of the wall or thicker "
              "than the glass stands on")
        # The top cell's hole reaches the cell's bottom edge - the seam -
        # because the glass runs down into this cell from above the sill.
        check(top[1] + top[3] == CELL_HEIGHT,
              "windowframe-TS's hole stops "
              f"{CELL_HEIGHT - (top[1] + top[3])}px short of its cell's "
              "bottom edge, which lays a transom across the glass at the "
              "join with the level below")
        check(top[1] == CELL_HEIGHT - math.floor(0.5 * CELL_HEIGHT),
              "windowframe-TS's hole does not reach 0.5 of a cell above its "
              f"cell's floor line: it starts at {top[1]}px from the cell "
              "top, and the span's glass tops out 0.5 of a level above the "
              "top cell's floor wherever the span ends")
        # The head and the single-level band are the same hole down to the
        # head moulding: `T` shares `S`'s hole *top* and differs from it
        # only at the bottom, where `S` stops at the 0.2 sill band and `T`
        # runs on to the cell's bottom edge. Stating it against the `S`
        # tile rather than only against the number is what stops the two
        # drifting apart one row at a time.
        single_top = hole_of("windowframe-SS")[1]
        check(top[1] == single_top,
              "windowframe-TS's hole starts "
              f"{top[1] - single_top:+d}px away from windowframe-SS's, so the "
              "head does not sit where the single-level band does: the glass "
              "tops out at the same height above the cell either way, and only "
              "the sill makes the two differ")
        for corner in ("windowframe-TL", "windowframe-TM", "windowframe-TR"):
            check(hole_of(corner)[1] == single_top,
                  f"{corner}'s hole does not share the single-level band's "
                  f"top: it starts at {hole_of(corner)[1]}px, the band at "
                  f"{single_top}px")
        # The middle cell carries jambs only: no head, no sill, so its
        # hole is the whole cell height and nothing crosses the join at
        # either of its edges.
        check(middle[1] == 0 and middle[1] + middle[3] == CELL_HEIGHT,
              "windowframe-MS's hole does not fill its cell vertically, so a "
              "middle level of a tall window carries a head or a sill "
              "through the middle of the glass")
        # Same horizontal band on all three, or the joined aperture is not
        # a rect up the column.
        check(bottom[0] == middle[0] == top[0]
                and bottom[2] == middle[2] == top[2],
              "the tall column's holes are not the same horizontal band, so "
              "the joined aperture is not rectangular up the span")
        # The joined holes against the glass rect of the whole span: a
        # 2-level span's glass is v[0.2, 1.5] above the span's floor line,
        # a 3-level span's is v[0.2, 2.5], each rounded down at both ends.
        for levels, parts in ((2, (bottom, top)),
                             (3, (bottom, middle, top))):
            joined = sum(part[3] for part in parts)
            want = (math.floor((levels - 0.5) * CELL_HEIGHT)
                    - math.floor(0.2 * CELL_HEIGHT))
            check(joined == want,
                  f"a {levels}-level span's holes joined are {joined}px tall; "
                  f"the span's glass rect v[0.2,{levels - 0.5}] rounds down "
                  f"to {want}px")

    # The corners and interior edges join the two axes into one aperture.
    # A corner carries both mouldings, and the test that matters is that
    # carrying both does not move the hole off the seam on either side: a
    # `BL` that stopped short of its top edge would leave a bar across the
    # glass where the corner meets the level above, and one that stopped
    # short of its right edge would put a vertical moulding inside the
    # opening at the join with `BM`.
    corners = {name: hole_of(name) for name in
              ("windowframe-BL", "windowframe-BM", "windowframe-BR",
               "windowframe-ML", "windowframe-MR",
               "windowframe-TL", "windowframe-TM", "windowframe-TR")
              if name in overlays}
    if len(corners) == 8:
        bl, bm, br = corners["windowframe-BL"], corners["windowframe-BM"], corners["windowframe-BR"]
        ml, mr = corners["windowframe-ML"], corners["windowframe-MR"]
        tl, tm, tr = corners["windowframe-TL"], corners["windowframe-TM"], corners["windowframe-TR"]

        # Each corner is inset on its own end of each axis and runs to the
        # seam on the two sides that face into the span.
        check(bl[0] == math.ceil(0.1 * CELL_WIDTH) and bl[1] == 0
                and bl[0] + bl[2] == CELL_WIDTH and bl[1] + bl[3] == 256,
              f"windowframe-BL's hole {bl} is not the sill-and-jamb corner: "
              "it should be inset 0.1 at the left, stop 0.2 above the floor "
              "line, and run to the cell's top and right edges")
        check(br[0] == 0 and br[1] == 0
                and br[0] + br[2] == math.floor(0.9 * CELL_WIDTH)
                and br[1] + br[3] == 256,
              f"windowframe-BR's hole {br} is not the sill-and-jamb corner: "
              "it should run from the cell's left edge, be inset 0.1 at the "
              "right, stop 0.2 above the floor line, and reach the cell's "
              "top edge")
        check(tl[0] == math.ceil(0.1 * CELL_WIDTH)
                and tl[0] + tl[2] == CELL_WIDTH
                and tl[1] + tl[3] == CELL_HEIGHT,
              f"windowframe-TL's hole {tl} is not the head-and-jamb corner: "
              "it should be inset 0.1 at the left and run to the cell's "
              "right and bottom edges")
        check(tr[0] == 0 and tr[0] + tr[2] == math.floor(0.9 * CELL_WIDTH)
                and tr[1] + tr[3] == CELL_HEIGHT,
              f"windowframe-TR's hole {tr} is not the head-and-jamb corner: "
              "it should run from the cell's left edge, be inset 0.1 at the "
              "right, and reach the cell's bottom edge")
        # The interior edges carry one moulding only: ML and MR are jamb
        # only over the whole cell height, and BM and TM are sill and head
        # only over the whole cell width.
        check(ml[0] == math.ceil(0.1 * CELL_WIDTH) and ml[1] == 0
                and ml[1] + ml[3] == CELL_HEIGHT,
              "windowframe-ML's hole is not jamb-only over the whole cell "
              "height, so a middle level of a wide tall span carries a head "
              "or a sill it has no business carrying")
        check(mr[0] == 0 and mr[1] == 0
                and mr[1] + mr[3] == CELL_HEIGHT,
              "windowframe-MR's hole is not jamb-only over the whole cell "
              "height")
        check(bm[0] == 0 and bm[0] + bm[2] == CELL_WIDTH
                and bm[1] == 0 and bm[1] + bm[3] == 256,
              "windowframe-BM's hole is not the sill alone across the full "
              "cell width")
        check(tm[0] == 0 and tm[0] + tm[2] == CELL_WIDTH
                and tm[1] + tm[3] == CELL_HEIGHT,
              "windowframe-TM's hole is not the head alone across the full "
              "cell width")

        # The bottom row of a wide tall span joins to the same width the
        # single-level wide row does, because the span's insets are the
        # same on both rows - only the vertical band differs.
        for row_name, row in (("bottom", (bl, bm, br)),
                             ("top", (tl, tm, tr))):
            for cells, parts in ((2, (row[0], row[2])),
                                (3, row)):
                joined = sum(part[2] for part in parts)
                want = (int((cells - 0.1) * CELL_WIDTH)
                       - math.ceil(0.1 * CELL_WIDTH))
                check(joined == want,
                      f"a {cells}-cell span's {row_name} row joins to "
                      f"{joined}px; the span's glass rect rounds down to "
                      f"{want}px, so the corner and the interior edge do "
                      "not line up with the ends of the span")
            check(len({part[1] for part in row}) == 1
                    and len({part[3] for part in row}) == 1,
                  f"the {row_name} row of a wide tall span is not one "
                  "horizontal band, so the joined aperture is not a rect")
        # Up the side columns: BL + ML + TL is the same 736px the single
        # -column tall span makes, because the vertical insets are the
        # same and only the horizontal band differs.
        for side, column in (("left", (bl, ml, tl)), ("right", (br, mr, tr))):
            for levels, parts in ((2, (column[0], column[2])),
                                 (3, column)):
                joined = sum(part[3] for part in parts)
                want = (math.floor((levels - 0.5) * CELL_HEIGHT)
                       - math.floor(0.2 * CELL_HEIGHT))
                check(joined == want,
                      f"a {levels}-level span's {side} column joins to "
                      f"{joined}px tall; the span's glass rect rounds down "
                      f"to {want}px, so the corner and the interior edge "
                      "do not line up with the ends of the span")
            # Every tile of a side column is inset the same at that side of
            # the span, or the jamb is not a straight line up the opening.
            if side == "left":
                check({part[0] for part in column} == {math.ceil(0.1 * CELL_WIDTH)},
                      "the left column's holes are not all inset 0.1 at the "
                      "span's left end, so the jamb does not run straight up "
                      "the opening")
            else:
                check({part[0] + part[2] for part in column}
                      == {math.floor(0.9 * CELL_WIDTH)},
                      "the right column's holes do not all reach the same "
                      "place at the span's right end, so the jamb does not "
                      "run straight up the opening")

    # Nobody tried to read the Furniture role convention into the block.
    # `<piece>-<role><level>` means the height level within a footprint;
    # this file's vertical role is end-versus-middle, and a digit-suffixed
    # windowframe name would give one convention two meanings in two
    # files.
    for name in authored:
        check(not re.search(r"-\d+$", name),
              f"{name} carries a trailing level digit, which is the "
              "Furniture role convention; a windowframe's vertical role "
              "is end-versus-middle, not a level index")

    # The committed placeholder sheet has the hole where the tileset says.
    # A tileset that states a hole and a sheet that never got punched is
    # the same wrong picture as no hole at all, and only one of the two
    # is visible in the file that was authored.
    image_path = (ROOT / SECTOR.tileset).parent / tileset["image"]
    with Image.open(image_path) as sheet:
        sheet = sheet.convert("RGBA")
        check(sheet.size == (width, height),
              f"the committed {tileset['image']} is {sheet.size[0]}x"
              f"{sheet.size[1]}, the tileset states {width}x{height}")
        for name in GLASS_RECTS:
            sprite = overlays.get(name)
            if sprite is None:
                continue
            column, row = sprite["cell"]
            hx, hy, hw, hh = sprite["hole"]
            cell_x, cell_y = column * CELL_WIDTH, row * CELL_HEIGHT
            ox, oy = cell_x + hx, cell_y + hy
            # The moulding just outside the opening, where the opening does
            # not run to the cell's own edge. Where the hole *does* reach
            # the cell edge - the seam side of every wide-row tile, both
            # sides of the middle column, and the seam sides of every
            # corner and interior edge - there is no neighbour inside this
            # cell to probe, and the pixel beyond belongs to the next
            # tile's art rather than to this frame's moulding, so it is
            # skipped. That the hole reaches the edge at all is pinned by
            # the joined-span checks above, not by probing here.
            for (px, py, expect_open) in (
                    (ox, oy, True),
                    (ox + hw - 1, oy + hh - 1, True),
                    (ox - 1, oy, False),
                    (ox + hw, oy, False),
                    (ox, oy - 1, False),
                    (ox, oy + hh, False)):
                if not (cell_x <= px < cell_x + CELL_WIDTH
                        and cell_y <= py < cell_y + CELL_HEIGHT):
                    continue
                alpha = sheet.getpixel((px, py))[3]
                if expect_open:
                    check(alpha == 0,
                          f"{name}: pixel ({px},{py}) is inside the framed "
                          f"opening and should be transparent, alpha {alpha}")
                else:
                    check(alpha == 255,
                          f"{name}: pixel ({px},{py}) is moulding beside the "
                          "opening and should be opaque, alpha {alpha}")

    # The manifest declares the frames, so the install pairs them and the
    # renderer can reach them.
    with (ROOT / "resources/Resources.yaml").open(encoding="utf-8") as stream:
        manifest = yaml.safe_load(stream)
    declared = {entry["name"]
                for entry in atlas_new.manifest_entries(manifest, "SectorAtlas")}
    for name in GLASS_RECTS:
        check(name in declared,
              f"{name} is authored but the SectorAtlas ImageSet does not "
              "declare it, so nothing installs it")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print(f"windowframe tiles: {len(GLASS_RECTS)} frames - the whole role "
          "grid less its all-middle cell - each holed inside its glass rect, "
          "punched in the committed sheet, laid out as the role grid and "
          "declared in the manifest; the grid closes here")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
