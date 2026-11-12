#!/usr/bin/env python3
"""The windowframe tiles the shipped Sector atlas authors (#1075).

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
grid off the sheet rather than learning a numbering. Only the
single-column, single-level cell is filled so far - the 1x1 Window, which
is eight of the ten windows every bundled document authors - and the
rest of the grid arrives with the wide, tall and corner work.

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
"""

from __future__ import annotations

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
# and top measured up from its cell's floor line. A 1x1 Window is inset
# 0.1 at each end of its span, stands 0.2 above the span's floor line
# and is 0.3 of a level tall, and every one of those is constant per role
# however big the span gets - which is what lets a fixed tile cover the
# opening whatever the opening's total size turns out to be.
GLASS_RECTS = {
    "windowframe-SS": (0.1, 0.9, 0.2, 0.5),
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
}

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
        for name, rect in GLASS_RECTS.items():
            sprite = overlays.get(name)
            if sprite is None:
                continue
            column, row = sprite["cell"]
            hx, hy, hw, hh = sprite["hole"]
            cell_x, cell_y = column * CELL_WIDTH, row * CELL_HEIGHT
            ox, oy = cell_x + hx, cell_y + hy
            # The moulding just outside the opening, where the opening
            # does not run to the cell's own edge. Every one of this
            # block's holes is inset on all four sides, so all four
            # neighbours here are this tile's own moulding rather than the
            # next tile's art.
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
                          f"opening and should be opaque, alpha {alpha}")

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
    print(f"windowframe tiles: {len(GLASS_RECTS)} frames, each holed inside "
          "its glass rect, punched in the committed sheet, laid out as the "
          "role grid and declared in the manifest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
