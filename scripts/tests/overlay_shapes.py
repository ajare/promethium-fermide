#!/usr/bin/env python3
"""The generator's check mode agrees with the loader on every shape of
`overlays:` (#1066).

One authored key is read twice by two readers that must not disagree. The
C++ loader fills the installed atlas; this generator writes the sheet and
the `ImageSet` block the manifest declares, and `--check` is what tells an
author the committed files are good. When the generator read a bodiless
`overlays:` as no overlays and the loader read it as a broken file, the
author got a clean `--check` and then a load-time refusal naming a map
they had written - in the worst place, after the files had been declared
good.

Every shape of the key is driven here through the real `--check`, against
a temp tree built from the same ten-surface fixture the loader test uses:
the sheet is rendered, a manifest is written holding exactly the regions
the shape should author, and `--check` has to agree. The loader's half of
the table is `sectorAtlasNew/loaderReadsEveryEmptyShapeOfOverlaysAsNoOverlays`
and `sectorAtlasNew/loaderRefusesAnOverlaysKeyThatIsNotAMap` in
`headless/src/smoke/render/SectorAtlasNew.cpp`. The two lists together are
the agreement: a shape added to either reader belongs in both.
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import atlas_new  # noqa: E402
from generate_sector_atlas_new import SPEC as SECTOR  # noqa: E402

TILE_WIDTH, TILE_HEIGHT = 128, 320

# The ten surface-bearing Sector types, in the columns the fixture lays them
# out in: one strip across the top of a two-row sheet, the second row left
# free for an overlay to stand in.
SURFACES = ["room", "corridor", "ladder", "lift", "shuttle", "stairwell",
            "staircase", "airlock", "chamber", "dumbwaiter"]

FRAME = "doorframe-room-1r-L0"

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def tileset_text(clause: str | None) -> str:
    """A conforming ten-surface version 2 tileset whose `overlays` key is
    written as `clause`. A `None` clause leaves the key out of the file."""
    lines = [
        "version: 2",
        "image: invented.png",
        f"size: [{len(SURFACES) * TILE_WIDTH}, {2 * TILE_HEIGHT}]",
        f"tile_size: [{TILE_WIDTH}, {TILE_HEIGHT}]",
        "sprites:",
    ]
    for column, name in enumerate(SURFACES):
        lines += [
            f"  {name}:",
            f"    cell: [{column}, 0]",
            f"    content: [0, 0, {TILE_WIDTH}, {TILE_HEIGHT}]",
            "    offset: [0.0, 0.0]",
            "    drawn_size: [1.0, 1.0]",
        ]
    if clause is not None:
        lines.append(clause.rstrip("\n"))
    return "\n".join(lines) + "\n"


POPULATED = "\n".join([
    "overlays:",
    f"  {FRAME}:",
    "    cell: [0, 1]",
    f"    content: [0, 0, {TILE_WIDTH}, {TILE_HEIGHT}]",
    "    offset: [0.0, 0.0]",
    "    drawn_size: [1.0, 1.0]",
])


def expected_regions(frames: int) -> list[tuple[str, int, int, int, int]]:
    """The regions the sheet authors: the ten surfaces, plus the frame when
    the shape holds one."""
    regions = [(name, column * TILE_WIDTH, 0, TILE_WIDTH, TILE_HEIGHT)
               for column, name in enumerate(SURFACES)]
    if frames:
        regions.append((FRAME, 0, TILE_HEIGHT, TILE_WIDTH, TILE_HEIGHT))
    return sorted(regions)


def manifest_text(entries: list[tuple[str, int, int, int, int]]) -> str:
    """A manifest registering the SectorAtlas `ImageSet` with `entries`."""
    lines = ["Resources:", "  Resource:", '    - name: "SectorAtlas"',
             "      Definitions:", "        Definition:", "          Images:",
             "            Image:"]
    for name, x, y, width, height in entries:
        lines.append(f'              - {{ name: "{name}", x: "{x}", y: "{y}", '
                     f'width: "{width}", height: "{height}" }}')
    return "\n".join(lines) + "\n"


def spec_for(tileset_path: pathlib.Path) -> atlas_new.AtlasSpec:
    """The shipped sector spec, pointed at a temp tileset instead of the
    committed one. `AtlasSpec.tileset_path` resolves an absolute path to
    itself, so the whole generator runs against the temp tree."""
    return atlas_new.AtlasSpec(
        atlas="SectorAtlas",
        tileset=str(tileset_path),
        script="generate_sector_atlas_new.py",
        label_tiles=True,
        variant_families=True,
    )


def run_check(spec: atlas_new.AtlasSpec,
             manifest: pathlib.Path) -> tuple[int, str]:
    """The generator's `--check` mode exactly as CI and the author run it,
    against a temp manifest instead of `resources/Resources.yaml`."""
    saved_argv, saved_manifest = sys.argv, atlas_new.MANIFEST
    out, err = io.StringIO(), io.StringIO()
    try:
        sys.argv = ["generate_sector_atlas_new.py", "--check"]
        atlas_new.MANIFEST = manifest
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = atlas_new.run(spec)
    except SystemExit as exit_:
        code = exit_.code if isinstance(exit_.code, int) else 1
        if isinstance(exit_.code, str):
            err.write(exit_.code)
    finally:
        sys.argv, atlas_new.MANIFEST = saved_argv, saved_manifest
    return code, out.getvalue() + err.getvalue()


# Every shape of the `overlays` key, and what the generator has to say
# about it. `frames` is how many overlay tiles the shape authors.
SHAPES = [
    # tag, the clause written into the file, accepted?, frames authored
    ("absent", None, True, 0),
    ("null", "overlays:\n", True, 0),
    ("empty-map", "overlays: {}\n", True, 0),
    ("populated", POPULATED, True, 1),
    ("scalar", "overlays: 3\n", False, 0),
    ("empty-string", 'overlays: ""\n', False, 0),
    ("sequence", f"overlays: [{FRAME}]\n", False, 0),
]

for tag, clause, accepted, frames in SHAPES:
    with tempfile.TemporaryDirectory(prefix=f"pf-overlay-shapes-{tag}-") as tmp:
        root = pathlib.Path(tmp)
        tileset_path = root / f"{tag}.tileset.yaml"
        tileset_path.write_text(tileset_text(clause), encoding="utf-8")
        spec = spec_for(tileset_path)
        tileset = atlas_new.load_tileset(spec)
        expected = expected_regions(frames)

        # The generator's own reading of the key, taken straight from the
        # function both `--check` and `--write` go through.
        refusal: str | None = None
        try:
            overlays = atlas_new.overlay_sprites(spec, tileset)
        except SystemExit as exit_:
            overlays, refusal = None, str(exit_.code)

        # The stand-in committed artifacts: the sheet at the size the
        # tileset states, and a manifest holding exactly the regions the
        # shape is supposed to author.
        if accepted:
            atlas_new.render_placeholder(spec, tileset).save(
                tileset_path.parent / tileset["image"])
        manifest = root / "Resources.yaml"
        manifest.write_text(manifest_text(expected), encoding="utf-8")

        code, output = run_check(spec, manifest)

        if accepted:
            check(refusal is None,
                  f"`{tag}` overlays: the generator refused a shape the loader "
                  f"accepts: {refusal}")
            check(code == 0,
                  f"`{tag}` overlays: `--check` failed a shape the loader "
                  f"accepts (exit {code}): {output.strip()}")
            if refusal is None:
                check(len(overlays) == frames,
                      f"`{tag}` overlays: the generator read "
                      f"{len(overlays)} overlay tiles; the shape authors "
                      f"{frames}")
            authored = [name for name, *_ in atlas_new.resolve_regions(
                spec, tileset)]
            check(authored == [name for name, *_ in expected],
                  f"`{tag}` overlays: the generator resolves {authored}, "
                  f"expected {[name for name, *_ in expected]}")
            if frames == 0:
                check(not [n for n in authored if n.startswith("doorframe-")],
                      f"`{tag}` overlays: the shape authors no overlay and the "
                      f"generator resolved a doorframe tile anyway")
        else:
            check(refusal is not None,
                  f"`{tag}` overlays: the generator accepted a shape the loader "
                  f"refuses")
            check(refusal is not None and "overlays" in refusal,
                  f"`{tag}` overlays: the refusal does not name the `overlays` "
                  f"key: {refusal}")
            check(code != 0,
                  f"`{tag}` overlays: `--check` passed a shape the loader "
                  f"refuses (exit {code}): {output.strip()}")

# The three empty shapes are not merely tolerated one at a time, they read
# identically: an author who stubs the map before filling it in gets the
# same answer as one who never wrote the key, and no doorframe tiles until
# they write some.
check(atlas_new.overlay_sprites(SECTOR, {"overlays": None})
      == atlas_new.overlay_sprites(SECTOR, {"overlays": {}})
      == atlas_new.overlay_sprites(SECTOR, {}),
      "the absent key, a Null and an empty map do not read the same")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    raise SystemExit(1)

print("PASS: the generator's check mode agrees with the loader on every shape "
      "of `overlays:`")
