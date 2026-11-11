#!/usr/bin/env python3
"""Shared generation for the `New` atlas sets (#1014, #1016).

Every `New` atlas is authored once, as a version 2 tileset YAML file. This
module turns that one authored file into the two artifacts the application
consumes:

  * the packed placeholder PNG at the image size the tileset states, and
  * the `ImageSet` pixel-region block the application manifest declares
    for that atlas.

The `ImageSet` stays the runtime pixel-region contract ADR 0010 names; what
it no longer is is hand-maintained. The render smoke module checks the
committed manifest against the tileset on every run, so a drift fails the
build rather than showing up as artwork in the wrong place.

Each atlas gets its own generated block in the manifest, marked with the
tileset it was generated from, so several atlases can be generated into one
manifest without colliding.

Modes:
  --check   verify the PNG and the manifest block already match the tileset
            (the default, and what CI runs)
  --write   regenerate the PNG and rewrite the generated block in
            resources/Resources.yaml

Requires Pillow and PyYAML.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from dataclasses import dataclass

import yaml
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "resources/Resources.yaml"
END = "# END GENERATED"


@dataclass(frozen=True)
class AtlasSpec:
    """One `New` atlas: the resource it fills, the tileset that authors it,
    and the script that generates it (named in the generated block so the
    block says where it came from)."""

    atlas: str
    tileset: str
    script: str
    # Placeholder art for a full-cell sheet is unreadable without the tile
    # name on it; a sheet of small tiles does not have room for one.
    label_tiles: bool = False
    # Whether this atlas's format lays out variant family columns. A base
    # sprite's `variants: N` promises N more tiles stacked directly beneath
    # it, and only a format with somewhere to put them can keep that promise.
    # Where the format has no column to grow, the key is refused rather than
    # ignored - an ignored declaration is a family the author believes
    # exists and nothing derives (#1038).
    variant_families: bool = False

    @property
    def tileset_path(self) -> pathlib.Path:
        return ROOT / self.tileset

    @property
    def begin_marker(self) -> str:
        return (f"# GENERATED from {self.tileset} by scripts/{self.script}. "
                "Do not hand-edit.")


# The placeholder slab steps one shade darker per sheet row, so the row a
# tile landed in reads off the sheet itself. That is what makes a variant
# that derived into the wrong row visible as artwork rather than only as a
# diff (#1038). Row 0 keeps the shade the placeholder has always used, so
# the base strip looks the same as it did before families existed.
ROW_SHADE_STEP = 40


def slab_shades(row: int, tintable: bool) -> tuple[tuple[int, ...], tuple[int, ...]]:
    """The placeholder slab body and edge for one sheet row, one step darker
    than the row above it. The hue is kept, so a tintable tile stays a
    grayscale tile and a non-tintable one stays the blue-grey it was."""
    body_base = (236, 236, 236) if tintable else (150, 160, 176)
    edge_base = (198, 198, 198) if tintable else (110, 120, 138)
    step = ROW_SHADE_STEP * row
    body = tuple(max(16, channel - step) for channel in body_base) + (255,)
    edge = tuple(max(8, channel - step) for channel in edge_base) + (255,)
    return body, edge


def load_tileset(spec: AtlasSpec) -> dict:
    path = spec.tileset_path
    with path.open(encoding="utf-8") as stream:
        tileset = yaml.safe_load(stream)
    if not isinstance(tileset, dict) or tileset.get("version") != 2:
        raise SystemExit(f"{path}: expected a version 2 authored tileset")
    width, height = tileset["size"]
    tile_width, tile_height = tileset["tile_size"]
    if width % tile_width or height % tile_height:
        raise SystemExit(f"{path}: the authored grid leaves a remainder in the image")
    return tileset


def is_reserved_variant_name(name: str) -> bool:
    """`-V` followed by digits and nothing else: exactly the shape the variant
    derivation produces."""
    mark = name.rfind("-V")
    if mark == -1:
        return False
    tail = name[mark + 2:]
    return bool(tail) and all(c in "0123456789" for c in tail)


def derive_sprites(spec: AtlasSpec, tileset: dict) -> dict:
    """Every sprite the sheet holds, with each declared variant family expanded.

    A base sprite's `-Vn` sits in the base's own column one row below the
    tile above it, and inherits the base's content rectangle, render offset,
    drawn size and tintable flag. The name and the cell both come from the
    rule rather than from the file, so neither can be written wrong (#1038).

    A derived tile carries no `variants` key of its own: the key belongs on
    the base that stated the family, and a family under a family has no
    column of its own to live in.
    """
    rows = tileset["size"][1] // tileset["tile_size"][1]
    derived: dict[str, dict] = {}
    for name, sprite in tileset["sprites"].items():
        # `-Vn` is the derivation's own vocabulary. A sprite authored under one
        # of those names would collide with whatever the family rule derives,
        # and which of the two won would come down to file order, so the name is
        # refused before anything is derived. The C++ loader refuses it the same
        # way; a generator that let it through would emit a manifest the loader
        # then rejects (#1038).
        if spec.variant_families and is_reserved_variant_name(name):
            raise SystemExit(f"{spec.tileset}: sprite '{name}' uses a name the "
                            "variant derivation reserves; a family's variants are "
                            "derived from its base and are not authored beside it")
        if name in derived:
            raise SystemExit(f"{spec.tileset}: '{name}' is both a derived variant "
                            "and an authored sprite; variant names come from the "
                            "family rule and cannot be authored beside it")
        derived[name] = sprite
        if "variants" not in sprite:
            continue
        if not spec.variant_families:
            raise SystemExit(f"{spec.tileset}: sprite '{name}' declares `variants`, "
                            f"which {spec.atlas} has no family column to derive; "
                            "only the sector tileset stacks variants beneath a base")
        count = sprite["variants"]
        if isinstance(count, bool) or not isinstance(count, int):
            raise SystemExit(f"{spec.tileset}: sprite '{name}' `variants` must be a "
                            "whole number")
        if count < 0:
            raise SystemExit(f"{spec.tileset}: sprite '{name}' `variants` must not "
                            "be negative")
        column, row = sprite["cell"]
        # The family descends from the base's own row, so its last variant
        # sits at row + count. Past the last row of the declared sheet there
        # is no cell for it, and a sheet that quietly stopped short of what
        # was asked is the silent failure a refusal is here to avoid.
        if row + count >= rows:
            raise SystemExit(f"{spec.tileset}: sprite '{name}' declares {count} "
                            f"variants, which would run past the bottom of the "
                            f"declared sheet: the base sits at row {row} of {rows}")
        for variant in range(1, count + 1):
            variant_name = f"{name}-V{variant}"
            if variant_name in derived:
                raise SystemExit(f"{spec.tileset}: sprite '{variant_name}' is "
                                f"derived from '{name}' and cannot also be authored "
                                "beside the family it belongs to")
            inherited = {key: value for key, value in sprite.items()
                         if key != "variants"}
            inherited["cell"] = [column, row + variant]
            derived[variant_name] = inherited
    return derived


def resolve_regions(spec: AtlasSpec,
                   tileset: dict) -> list[tuple[str, int, int, int, int]]:
    """Every tile's region in image pixels: its cell plus its content offset.

    Variant families are expanded first, so the regions the manifest
    declares are the whole sheet's and not just the base sprites'.
    """
    tile_width, tile_height = tileset["tile_size"]
    regions = []
    for name, sprite in derive_sprites(spec, tileset).items():
        column, row = sprite["cell"]
        x, y, width, height = sprite["content"]
        regions.append((name, column * tile_width + x, row * tile_height + y,
                       width, height))
    return sorted(regions)


def render_placeholder(spec: AtlasSpec, tileset: dict) -> Image.Image:
    """Placeholder artwork at the stated dimensions: a light slab per tile.

    Tintable tiles are drawn in grayscale so a vertex tint multiplies into
    them faithfully; the real sheets replace this file without changing any
    of the geometry recorded in the tileset. Every tile - base or derived
    variant - is labelled with its own name, and the slab steps one shade
    darker per sheet row, so the sheet reads as the family structure it is.
    """
    width, height = tileset["size"]
    tile_width, tile_height = tileset["tile_size"]
    sprites = derive_sprites(spec, tileset)
    sheet = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sheet)
    for name, x, y, region_width, region_height in resolve_regions(spec, tileset):
        sprite = sprites[name]
        tintable = sprite.get("tintable", False)
        body, edge = slab_shades(sprite["cell"][1], tintable)
        draw.rectangle([x, y, x + region_width - 1, y + region_height - 1],
                       fill=body)
        draw.rectangle([x, y, x + region_width - 1, min(y + 1, y + region_height - 1)],
                       fill=edge)
        if spec.label_tiles:
            # The label goes on the cell, not on the content. A tile that
            # fills its cell - which every other labelled sheet is made of -
            # reads the same either way, and a tile that does not leaves its
            # cell's own space free to be written on. Labelling the content
            # instead puts a long name on top of the neighbouring tile's
            # art, which is the one thing a sheet should never do.
            column, row = sprite["cell"]
            draw.text((column * tile_width + 6, row * tile_height + 6), name,
                     fill=(52, 58, 70, 255))
    return sheet


def manifest_block(spec: AtlasSpec,
                  regions: list[tuple[str, int, int, int, int]]) -> list[str]:
    lines = [spec.begin_marker]
    for name, x, y, width, height in regions:
        lines.append(f'- {{ name: "{name}", x: "{x}", y: "{y}", '
                    f'width: "{width}", height: "{height}" }}')
    lines.append(END)
    return lines


def manifest_entries(manifest: dict, atlas: str) -> list[dict]:
    for resource in manifest["Resources"]["Resource"]:
        if not isinstance(resource, dict) or resource.get("name") != atlas:
            continue
        images = resource["Definitions"]["Definition"]["Images"]["Image"]
        return [{key: str(entry[key]) for key in ("name", "x", "y", "width", "height")}
                for entry in images]
    raise SystemExit(f"{MANIFEST}: no {atlas} ImageSet is registered")


def check_manifest(spec: AtlasSpec,
                  regions: list[tuple[str, int, int, int, int]]) -> list[str]:
    with MANIFEST.open(encoding="utf-8") as stream:
        manifest = yaml.safe_load(stream)
    expected = [{"name": name, "x": str(x), "y": str(y), "width": str(width),
                 "height": str(height)}
                for name, x, y, width, height in regions]
    actual = manifest_entries(manifest, spec.atlas)
    problems = []
    if actual != expected:
        problems.append(
            f"{MANIFEST.name}: {spec.atlas} entries do not match the authored "
            f"tileset\n  expected: {expected}\n  actual:   {actual}\n"
            f"  regenerate with: python3 scripts/{spec.script} --write")
    return problems


def check_image(spec: AtlasSpec, tileset: dict) -> list[str]:
    image_path = spec.tileset_path.parent / tileset["image"]
    if not image_path.is_file():
        return [f"{image_path}: the packed atlas image is missing"]
    with Image.open(image_path) as sheet:
        width, height = tileset["size"]
        if sheet.size != (width, height):
            return [f"{image_path}: is {sheet.size[0]}x{sheet.size[1]}, the "
                    f"tileset states {width}x{height}"]
    return []


def write_manifest(spec: AtlasSpec,
                  regions: list[tuple[str, int, int, int, int]]) -> None:
    text = MANIFEST.read_text(encoding="utf-8")
    lines = text.split("\n")
    start = next((i for i, line in enumerate(lines) if spec.begin_marker in line),
                None)
    if start is None:
        raise SystemExit(f"{MANIFEST}: no generated block for {spec.tileset} "
                        "is present to rewrite")
    end = next((i for i in range(start + 1, len(lines)) if END in lines[i]), None)
    if end is None:
        raise SystemExit(f"{MANIFEST}: the {spec.atlas} generated block is "
                        f"missing its `{END}` marker")
    indent = lines[start][:len(lines[start]) - len(lines[start].lstrip())]
    replacement = [indent + line for line in manifest_block(spec, regions)]
    lines[start:end + 1] = replacement
    MANIFEST.write_text("\n".join(lines), encoding="utf-8")


def run(spec: AtlasSpec) -> int:
    parser = argparse.ArgumentParser(
        description=f"Generate the {spec.atlas} sheet and manifest entries.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                     help="verify the PNG and the manifest match the tileset")
    mode.add_argument("--write", action="store_true",
                     help="regenerate the PNG and the manifest's generated block")
    parser.add_argument("--emit", action="store_true",
                       help="print the ImageSet entries and exit")
    arguments = parser.parse_args()

    tileset = load_tileset(spec)
    regions = resolve_regions(spec, tileset)

    if arguments.emit:
        for line in manifest_block(spec, regions):
            print(line)
        return 0

    if arguments.write:
        sheet = render_placeholder(spec, tileset)
        sheet.save(spec.tileset_path.parent / tileset["image"])
        write_manifest(spec, regions)
        print(f"wrote {spec.tileset_path.parent / tileset['image']} and the "
              f"{spec.atlas} entries in {MANIFEST.name}")
        return 0

    problems = check_image(spec, tileset) + check_manifest(spec, regions)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{spec.atlas} matches its authored tileset "
          f"({len(regions)} tiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit("atlas_new.py is a library; run "
                    "scripts/generate_object_atlas_new.py, "
                    "scripts/generate_furniture_atlas_new.py or "
                    "scripts/generate_sector_atlas_new.py")
