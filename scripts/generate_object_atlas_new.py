#!/usr/bin/env python3
"""Generate the ObjectAtlasNew sheet and the manifest entries that atlas owns.

The authored tileset is the single source of truth (#1014). This script
reads `resources/textures/objects-new.tileset.yaml`, writes the packed
placeholder PNG at the image size that file states, and emits the
`ImageSet` pixel regions the application manifest declares for the atlas.

The `ImageSet` stays the runtime pixel-region contract ADR 0010 names; what
it no longer is is hand-maintained. The render smoke module checks the
committed manifest against this tileset, so a drift fails the build.

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

import yaml
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
TILESET = ROOT / "resources/textures/objects-new.tileset.yaml"
MANIFEST = ROOT / "resources/Resources.yaml"
ATLAS_RESOURCE = "ObjectAtlasNew"
BEGIN = "# GENERATED from resources/textures/objects-new.tileset.yaml by"
END = "# END GENERATED"


def load_tileset(path: pathlib.Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        tileset = yaml.safe_load(stream)
    if not isinstance(tileset, dict) or tileset.get("version") != 2:
        raise SystemExit(f"{path}: expected a version 2 authored tileset")
    width, height = tileset["size"]
    tile_width, tile_height = tileset["tile_size"]
    if width % tile_width or height % tile_height:
        raise SystemExit(f"{path}: the authored grid leaves a remainder in the image")
    return tileset


def resolve_regions(tileset: dict) -> list[tuple[str, int, int, int, int]]:
    """Every tile's region in image pixels: its cell plus its content offset."""
    tile_width, tile_height = tileset["tile_size"]
    regions = []
    for name, sprite in tileset["sprites"].items():
        column, row = sprite["cell"]
        x, y, width, height = sprite["content"]
        regions.append((name, column * tile_width + x, row * tile_height + y,
                       width, height))
    return sorted(regions)


def render_placeholder(tileset: dict) -> Image.Image:
    """Placeholder artwork at the stated dimensions: a light slab per tile.

    Tintable tiles are drawn in grayscale so a vertex tint multiplies into
    them faithfully; the real sheets replace this file without changing any
    of the geometry recorded in the tileset.
    """
    width, height = tileset["size"]
    sheet = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sheet)
    for name, x, y, region_width, region_height in resolve_regions(tileset):
        tintable = tileset["sprites"][name].get("tintable", False)
        body = (236, 236, 236, 255) if tintable else (150, 160, 176, 255)
        edge = (198, 198, 198, 255) if tintable else (110, 120, 138, 255)
        draw.rectangle([x, y, x + region_width - 1, y + region_height - 1],
                      fill=body)
        draw.rectangle([x, y, x + region_width - 1, min(y + 1, y + region_height - 1)],
                      fill=edge)
    return sheet


def manifest_block(regions: list[tuple[str, int, int, int, int]]) -> list[str]:
    lines = [
        f"{BEGIN} scripts/generate_object_atlas_new.py. Do not hand-edit.",
    ]
    for name, x, y, width, height in regions:
        lines.append(f'- {{ name: "{name}", x: "{x}", y: "{y}", '
                    f'width: "{width}", height: "{height}" }}')
    lines.append(END)
    return lines


def manifest_entries(manifest: dict) -> list[dict]:
    for resource in manifest["Resources"]["Resource"]:
        if resource.get("name") != ATLAS_RESOURCE:
            continue
        images = resource["Definitions"]["Definition"]["Images"]["Image"]
        return [{key: str(entry[key]) for key in ("name", "x", "y", "width", "height")}
                for entry in images]
    raise SystemExit(f"{MANIFEST}: no {ATLAS_RESOURCE} ImageSet is registered")


def check_manifest(regions: list[tuple[str, int, int, int, int]]) -> list[str]:
    with MANIFEST.open(encoding="utf-8") as stream:
        manifest = yaml.safe_load(stream)
    expected = [{"name": name, "x": str(x), "y": str(y), "width": str(width),
                 "height": str(height)}
                for name, x, y, width, height in regions]
    actual = manifest_entries(manifest)
    problems = []
    if actual != expected:
        problems.append(
            f"{MANIFEST.name}: {ATLAS_RESOURCE} entries do not match the authored "
            f"tileset\n  expected: {expected}\n  actual:   {actual}\n"
            f"  regenerate with: python3 scripts/generate_object_atlas_new.py --write")
    return problems


def check_image(tileset: dict) -> list[str]:
    image_path = TILESET.parent / tileset["image"]
    if not image_path.is_file():
        return [f"{image_path}: the packed atlas image is missing"]
    with Image.open(image_path) as sheet:
        width, height = tileset["size"]
        if sheet.size != (width, height):
            return [f"{image_path}: is {sheet.size[0]}x{sheet.size[1]}, the "
                    f"tileset states {width}x{height}"]
    return []


def write_manifest(regions: list[tuple[str, int, int, int, int]]) -> None:
    text = MANIFEST.read_text(encoding="utf-8")
    lines = text.split("\n")
    start = next((i for i, line in enumerate(lines) if BEGIN in line), None)
    end = next((i for i, line in enumerate(lines) if END in line), None)
    if start is None or end is None or end < start:
        raise SystemExit(f"{MANIFEST}: no generated block found to rewrite")
    indent = lines[start][:len(lines[start]) - len(lines[start].lstrip())]
    replacement = [indent + line for line in manifest_block(regions)]
    lines[start:end + 1] = replacement
    MANIFEST.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                    help="verify the PNG and the manifest match the tileset")
    mode.add_argument("--write", action="store_true",
                    help="regenerate the PNG and the manifest's generated block")
    parser.add_argument("--emit", action="store_true",
                    help="print the ImageSet entries and exit")
    arguments = parser.parse_args()

    tileset = load_tileset(TILESET)
    regions = resolve_regions(tileset)

    if arguments.emit:
        for line in manifest_block(regions):
            print(line)
        return 0

    if arguments.write:
        sheet = render_placeholder(tileset)
        sheet.save(TILESET.parent / tileset["image"])
        write_manifest(regions)
        print(f"wrote {TILESET.parent / tileset['image']} and the "
              f"{ATLAS_RESOURCE} entries in {MANIFEST.name}")
        return 0

    problems = check_image(tileset) + check_manifest(regions)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{ATLAS_RESOURCE} matches its authored tileset "
          f"({len(regions)} tiles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
