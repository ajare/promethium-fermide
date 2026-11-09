#!/usr/bin/env python3
"""Generate the SectorAtlasNew sheet and the manifest entries it owns (#1017).

The authored tileset is the single source of truth. This script reads
`resources/textures/sectors-new.tileset.yaml`, writes the packed
placeholder PNG at the image size that file states, and emits the
`ImageSet` pixel regions the application manifest declares for the atlas.

The shared mechanics live in `scripts/atlas_new.py`, which every `New`
atlas generates through; this file states which atlas this one fills. The
placeholder tiles are labelled with their names because a sheet of ten
identical full-cell slabs says nothing to the artist otherwise - which
cell of the Infrastructure set a tile belongs to is the thing worth
reading off the sheet.

Modes:
  --check   verify the PNG and the manifest block already match the tileset
            (the default, and what CI runs)
  --write   regenerate the PNG and rewrite the generated block in
            resources/Resources.yaml

Requires Pillow and PyYAML.
"""

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from atlas_new import AtlasSpec, run  # noqa: E402

SPEC = AtlasSpec(
    atlas="SectorAtlasNew",
    tileset="resources/textures/sectors-new.tileset.yaml",
    script="generate_sector_atlas_new.py",
    label_tiles=True,
)

if __name__ == "__main__":
    sys.exit(run(SPEC))
