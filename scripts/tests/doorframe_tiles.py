#!/usr/bin/env python3
"""The doorframe tiles the shipped Sector atlas authors (#1054, #1073).

A doorframe is the moulding laid over the wall's own tile around the
opening a Door stands in. Its transparent hole is the opening the leaf
animates inside, and the hole is the one part of the artwork that cannot
be wrong by a fraction of a pixel in the wrong direction. The leaf is
clipped to the aperture, so a hole authored *smaller* than the aperture
costs at most a fraction of a pixel of moulding covered by the leaf; a
hole authored *larger* puts the moulding inside the doorway, which reads
as an opening smaller than the hole it was cut for.

So the rule is: round the hole down, inward on every edge, from the
aperture it frames. This checks that the authored hole is exactly that,
that the committed placeholder sheet really has the hole where the tileset
says, and that the frames are in the atlas's `overlays` map rather than
among its surfaces - which is what keeps `hasSectorTileSurface()`
answering "is this a Sector surface" and not "is this a tile".

The frames are keyed by the opening and by nothing else. Six tiles cover
every doorway in every wall type, because the frame lies *over* the wall's
tile and the wall's look comes from that tile: what is left of a frame to
draw once it is not the wall is the moulding, and the moulding does not
know a Room from a Corridor from a Facade. The fifteen wall-keyed frames
this replaced were three copies of artwork that could not tell the walls
apart, and they are refused here by name so they cannot come back as
though the wall still needed them.
"""

from __future__ import annotations

import pathlib
import sys

import yaml
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import atlas_new  # noqa: E402
from generate_sector_atlas_new import SPEC as SECTOR  # noqa: E402

CELL_WIDTH = 128
CELL_HEIGHT = 320

# The six frames, with the aperture each one frames in World units: the
# horizontal span across its cell and the height above the cell's floor
# line. A 2-wide opening spans two cells, and each cell's own span is what
# its frame has to open onto - the left cell from its 0.1 inset to its
# right edge, the right cell from its left edge to its 0.9 inset. Together
# they make the 1.8-wide aperture.
APERTURES = {
    "doorframe-1r-L0": (0.1, 0.9, 0.5),
    "doorframe-2r-L0": (0.1, 1.0, 0.5),
    "doorframe-2r-R0": (0.0, 0.9, 0.5),
    "doorframe-1t-L0": (0.1, 0.9, 0.9),
    "doorframe-2t-L0": (0.1, 1.0, 0.9),
    "doorframe-2t-R0": (0.0, 0.9, 0.9),
}

# The wall-keyed frames this replaced. They are refused rather than merely
# absent: a frame that lies over a wall has no use for the wall in its
# name, and one that came back would be three copies of a tile that cannot
# tell the copies apart.
FORBIDDEN = [
    f"doorframe-{wall}-{rest}"
    for wall in ("room", "corridor", "facade")
    for rest in ("1r-L0", "2r-L0", "2r-R0", "1t-L0", "2t-L0", "2t-R0")
]

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def load_tileset() -> dict:
    with (ROOT / SECTOR.tileset).open(encoding="utf-8") as stream:
        return yaml.safe_load(stream)


def expected_hole(aperture: tuple[float, float, float]) -> tuple[int, int, int, int]:
    """The hole the aperture allows, rounded down into whole pixels.

    Inward on every edge: the left edge rounds up, the right edge and the
    height round down. Never a pixel outside the aperture.
    """
    x0, x1, height = aperture
    left = -(-x0 * CELL_WIDTH // 1)          # ceil
    right = int(x1 * CELL_WIDTH)            # floor
    pixels = int(height * CELL_HEIGHT)      # floor
    return (left, CELL_HEIGHT - pixels, right - left, pixels)


def main() -> int:
    tileset = load_tileset()
    overlays = tileset.get("overlays")
    if not isinstance(overlays, dict):
        print("FAIL: the sector tileset has no `overlays` map for its "
              "doorframes")
        return 1

    check(set(overlays) == set(APERTURES),
          f"the authored overlays are {sorted(overlays)}, expected the six "
          f"opening-keyed frames {sorted(APERTURES)}")
    for name in FORBIDDEN:
        check(name not in overlays,
              f"{name} is authored under a wall key; a frame laid over the "
              "wall's own tile does not vary with the wall")

    # The frames are overlays, not surfaces: the map they are authored in
    # is what keeps `hasSectorTileSurface()` answering about Sector
    # surfaces alone.
    for name in APERTURES:
        check(name not in tileset.get("sprites", {}),
              f"{name} is authored among the surfaces; a doorframe is not a "
              "Sector surface")

    for name, aperture in APERTURES.items():
        sprite = overlays.get(name)
        if sprite is None:
            continue
        check("variants" not in sprite,
              f"{name} declares a variant family; a frame is derived from the "
              "opening it frames and has no family of pictures to pick from")
        hole = sprite.get("hole")
        check(isinstance(hole, list) and len(hole) == 4,
              f"{name} declares no `hole` rectangle")
        if not (isinstance(hole, list) and len(hole) == 4):
            continue
        want = expected_hole(aperture)
        check(tuple(hole) == want,
              f"{name} authors hole {tuple(hole)}; the aperture "
              f"u[{aperture[0]},{aperture[1]}] v[0,{aperture[2]}] rounds down "
              f"to {want}")
        # Stated the other way round, so a change to the rounding helper
        # cannot make both halves of this agree by moving together.
        x, y, width, height = hole
        check(x / CELL_WIDTH >= aperture[0] - 1e-9
              and (x + width) / CELL_WIDTH <= aperture[1] + 1e-9
              and height / CELL_HEIGHT <= aperture[2] + 1e-9,
              f"{name}'s hole reaches outside the aperture it frames")

    # The committed placeholder sheet has the hole where the tileset says.
    # A tileset that states a hole and a sheet that never got punched is
    # the same wrong picture as no hole at all, and only one of the two is
    # visible in the file that was authored.
    image_path = (ROOT / SECTOR.tileset).parent / tileset["image"]
    with Image.open(image_path) as sheet:
        sheet = sheet.convert("RGBA")
        for name, aperture in APERTURES.items():
            sprite = overlays.get(name)
            if sprite is None:
                continue
            column, row = sprite["cell"]
            hx, hy, hw, hh = sprite["hole"]
            cell_x, cell_y = column * CELL_WIDTH, row * CELL_HEIGHT
            ox, oy = cell_x + hx, cell_y + hy
            # The moulding just outside the opening, where the opening does
            # not run to the cell's own edge. A 2-wide frame's hole reaches
            # the shared edge of its two cells, and the pixel past it is
            # the neighbour tile's art - opaque, but not this tile's
            # moulding, so checking it would prove nothing about this one.
            for (px, py, expect_open) in (
                    (ox, oy, True),
                    (ox + hw - 1, oy + hh - 1, True),
                    (ox - 1, oy, False),
                    (ox + hw, oy, False),
                    (ox, oy - 1, False)):
                if not expect_open and not (cell_x <= px < cell_x + CELL_WIDTH
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
    for name in APERTURES:
        check(name in declared,
              f"{name} is authored but the SectorAtlas ImageSet does not "
              "declare it, so nothing installs it")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print(f"doorframe tiles: {len(APERTURES)} frames, each holed inside its "
          "aperture, punched in the committed sheet and declared in the "
          "manifest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
