#!/usr/bin/env python3
"""The `New` atlas generator derives the variant families the loader derives (#1053).

One authored tileset is read twice: by the C++ grid loader, which fills the
installed atlas, and by this generator, which writes the sheet and the
`ImageSet` block the manifest declares. The install pairs from the manifest
inward, so a family the loader derived but the generator left out of the
manifest is a family nothing installs, and one the generator emitted but the
loader refused is a manifest the application will not load. Both readers have
to run off the same rule.

The sector atlas has proved that end to end since #1038, with derived tiles
committed in its sheet and manifest. The object-shaped atlases took the
capability in #1053 and ship no family of their own yet, so nothing
committed proves their half of the agreement. This does, against the shipped
specs rather than a copy of one, and through the same functions `--check`
and `--write` run.

Every case is a refusal or a placement, because those are the two things the
rule is made of: where a variant lands, and what an author may not write.
"""

from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import atlas_new  # noqa: E402
from generate_agent_atlas_new import SPEC as AGENT  # noqa: E402
from generate_furniture_atlas_new import SPEC as FURNITURE  # noqa: E402
from generate_object_atlas_new import SPEC as OBJECT  # noqa: E402
from generate_sector_atlas_new import SPEC as SECTOR  # noqa: E402

TILE_WIDTH, TILE_HEIGHT = 128, 320


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def refused(spec, tileset: dict, expectation: str) -> str:
    """The refusal text for a fixture the derivation must not accept."""
    try:
        atlas_new.derive_sprites(spec, tileset)
    except SystemExit as refusal:
        text = str(refusal)
        check(text != "", f"{expectation}: the refusal carried no message")
        return text
    raise SystemExit(f"FAIL: {expectation}: the fixture was accepted")


def refused_call(fn, expectation: str) -> str:
    """The refusal text for a call the rule must not let through.

    The same shape as `refused`, for the readers that are not
    `derive_sprites` - the overlay map goes through `overlay_sprites` and
    `all_sprites`, and both have to refuse the same names the derivation
    does (#1069).
    """
    try:
        fn()
    except SystemExit as refusal:
        text = str(refusal)
        check(text != "", f"{expectation}: the refusal carried no message")
        return text
    raise SystemExit(f"FAIL: {expectation}: the fixture was accepted")


def sheet(rows: int, sprites: dict) -> dict:
    """A four-column invented sheet, `rows` deep, holding `sprites`."""
    return {
        "version": 2,
        "image": "invented.png",
        "size": [4 * TILE_WIDTH, rows * TILE_HEIGHT],
        "tile_size": [TILE_WIDTH, TILE_HEIGHT],
        "sprites": sprites,
    }


def overlay_sheet(name: str, base_variants=None) -> dict:
    """The invented sheet: one `sprites` base and one overlay named `name`.

    The base declares no family unless asked for, so an overlay named
    `door-V9` collides with nothing that exists - only with the shape the
    format's derivation reserves (#1069).
    """
    fixture = sheet(4, {"door": base(column=0, row=0, variants=base_variants)})
    fixture["overlays"] = {
        name: {"cell": [1, 0], "content": [4, 8, 100, 200],
               "offset": [0.0, -0.075], "drawn_size": [1.0, 0.075],
               "tintable": True},
    }
    return fixture


def base(column: int = 1, row: int = 0, variants=None) -> dict:
    """One authored sprite with render semantics no variant may change."""
    sprite = {
        "cell": [column, row],
        "content": [4, 8, 100, 200],
        "offset": [0.0, -0.075],
        "drawn_size": [1.0, 0.075],
        "tintable": True,
    }
    if variants is not None:
        sprite["variants"] = variants
    return sprite


# Every `New` atlas generates variant families now. The object-shaped ones
# had to, because the loader that fills their slots derives them: a sheet
# that refused what its own loader accepts would write a manifest with the
# derived names missing, and the install would never reach them.
for spec in (OBJECT, AGENT, FURNITURE, SECTOR):
    check(spec.variant_families,
          f"{spec.atlas} does not generate variant families")

# A declared family expands downward through the base's own column, and each
# step carries the base's content rectangle, render offset, drawn size and
# tintable flag untouched. The count is the only thing the author stated.
fixture = sheet(4, {"door": base(column=2, row=0, variants=3)})
derived = atlas_new.derive_sprites(OBJECT, fixture)
check(set(derived) == {"door", "door-V1", "door-V2", "door-V3"},
      f"a family of three did not expand to three derived tiles: "
      f"{sorted(derived)}")
for step in (1, 2, 3):
    variant = derived[f"door-V{step}"]
    check(variant["cell"] == [2, step],
          f"door-V{step} left its base's column or its row: {variant['cell']}")
    for field in ("content", "offset", "drawn_size", "tintable"):
        check(variant[field] == fixture["sprites"]["door"][field],
              f"door-V{step} did not inherit its base's `{field}`")
    # The key belongs on the base that stated the family. A derived tile
    # carrying one would read as a family under a family, which has no
    # column of its own to live in.
    check("variants" not in variant,
          f"door-V{step} carries a `variants` key of its own")

# The regions the manifest declares are the derived tiles' as much as the
# base's: the base's column, one authored tile height per step down.
regions = {name: rest
           for name, *rest in atlas_new.resolve_regions(OBJECT, fixture)}
check(regions["door-V2"] == [2 * TILE_WIDTH + 4, 2 * TILE_HEIGHT + 8, 100, 200],
      f"door-V2 resolved to the wrong pixels: {regions['door-V2']}")

# And they reach the generated manifest block, which is the half the
# application actually reads.
block = atlas_new.manifest_block(OBJECT, atlas_new.resolve_regions(OBJECT, fixture))
check(any('- { name: "door-V3"' in line for line in block),
      "the generated manifest block carries no derived tile")
check(block[-1] == atlas_new.END,
      "the generated manifest block lost its end marker")

# The sheet itself is emitted for the derived tiles too, not only the
# manifest: the cell a variant landed in is painted, and the cells nothing
# was placed in stay transparent.
placeholder = atlas_new.render_placeholder(OBJECT, fixture)
check(placeholder.size == (4 * TILE_WIDTH, 4 * TILE_HEIGHT),
      f"the placeholder sheet is not the size the tileset states: "
      f"{placeholder.size}")
landed = placeholder.getpixel((2 * TILE_WIDTH + 64, 2 * TILE_HEIGHT + 160))
check(landed[3] == 255,
      "the cell door-V2 landed in is empty on the generated sheet")
vacant = placeholder.getpixel((64, 2 * TILE_HEIGHT + 160))
check(vacant[3] == 0,
      "a cell no tile was placed in came back painted on the generated sheet")

# A `-Vn` name is the derivation's to give. Authored beside a family - or
# anywhere else on a sheet that derives one - it would collide with whatever
# the rule produces, and which of the two won would come down to file order.
collision = sheet(4, {
    "door": base(column=0, row=0, variants=2),
    "door-V1": base(column=1, row=0),
})
text = refused(OBJECT, collision, "an authored `-Vn` name")
check("door-V1" in text and "reserves" in text,
      f"the refusal does not name the reserved name: {text}")

# The reservation reaches the overlays as well. An overlay derives nothing -
# a frame is not picked from a swatch strip - but the `-Vn` shape belongs
# to the derivation this same file runs over `sprites`, so an overlay named
# `doorframe-room-1r-L0-V1` reads as a family nothing made and the file
# says nothing is wrong with it. The both-maps collision in `all_sprites`
# catches only the names some existing family has already derived; `door-V9`
# collides with nothing at all and is refused by shape alone, which is the
# half the collision could never reach (#1069).
for reserved in ("doorframe-room-1r-L0-V1", "room-V1", "door-V9"):
    check(atlas_new.is_reserved_variant_name(reserved),
          f"`{reserved}` is not the reserved shape")
    text = refused_call(lambda name=reserved: atlas_new.all_sprites(
        OBJECT, overlay_sheet(name)),
        f"an overlay named `{reserved}`")
    check(reserved in text and "reserves" in text,
          f"the overlay name refusal neither names `{reserved}` nor says the "
          f"shape is reserved: {text}")

# A name merely near the shape is nobody's, and an overlay in a format that
# derives nothing anywhere is not refused by shape either: the reservation
# follows the derivation, and where the format has none there is nothing to
# reserve (#1069).
for plain in ("doorframe-room-1r-L0", "doorframe-VX", "door-V1x", "door-V"):
    check(not atlas_new.is_reserved_variant_name(plain),
          f"`{plain}` is not the reserved shape and must still load")
    tiles = atlas_new.all_sprites(OBJECT, overlay_sheet(plain))
    check(plain in tiles, f"an overlay named `{plain}` was not carried")

NO_DERIVATION = atlas_new.AtlasSpec(
    atlas="LegacyTheme", tileset=OBJECT.tileset,
    script="generate_object_atlas_new.py", variant_families=False)
carried = atlas_new.all_sprites(NO_DERIVATION, overlay_sheet("door-V1"))
check("door-V1" in carried,
      "an overlay wearing the `-Vn` shape was refused in a format that derives "
      "nothing anywhere, where no name shape is spoken for")

# The count has to be a whole number. A float, a word or a boolean is not
# the author stating a count, and deriving from one would either round a
# family away or invent one.
for value in ("two", 1.5, True, [1]):
    text = refused(OBJECT, sheet(4, {"door": base(variants=value)}),
                   f"a non-integer `variants` value ({value!r})")
    check("whole number" in text,
          f"a non-integer `variants` was not refused as one ({value!r}): {text}")

# A negative count describes a family climbing above its base, which is not
# what the column means.
text = refused(OBJECT, sheet(4, {"door": base(variants=-1)}),
               "a negative `variants` count")
check("negative" in text, f"a negative count was not refused as one: {text}")

# A family that runs past the bottom of the declared sheet describes a tile
# no cell exists for. The refusal names the base's row and the sheet's row
# count, because "it does not fit" without both numbers is not actionable.
text = refused(OBJECT, sheet(4, {"door": base(column=0, row=3, variants=1)}),
               "a family descending past the last row")
check("run past the bottom" in text and "row 3 of 4" in text,
      f"the past-the-bottom refusal does not name the row and the row count: "
      f"{text}")

# The boundary is inclusive of the last row: a base at row 0 of a four-row
# sheet may claim rows 1, 2 and 3, and no further.
fits = sheet(4, {"door": base(column=0, row=0, variants=3)})
check(len(atlas_new.derive_sprites(OBJECT, fits)) == 4,
      "a family that fills the sheet down to the last row was not accepted")
text = refused(OBJECT, sheet(4, {"door": base(column=0, row=0, variants=4)}),
               "a family one row past the last row")
check("run past the bottom" in text,
      f"a family one row past the last row was accepted: {text}")

# A sprite with no `variants` key is left exactly as authored, and a sheet
# with no families at all is unchanged by the pass - which is why flipping
# the flag on the shipped atlases moved nothing.
plain = sheet(2, {"walkway": base(column=0, row=0)})
check(atlas_new.derive_sprites(OBJECT, plain) == plain["sprites"],
      "a sheet with no declared families came back changed by the derivation")

print("PASS: the atlas generator derives variant families the loader derives")
