#!/usr/bin/env python3
"""Generate the ObjectAtlasNew sheet and the manifest entries it owns (#1014).

The authored tileset is the single source of truth. This script reads
`resources/textures/objects-new.tileset.yaml`, writes the packed
placeholder PNG at the image size that file states, and emits the
`ImageSet` pixel regions the application manifest declares for the atlas.

The shared mechanics live in `scripts/atlas_new.py`, which every `New`
atlas generates through; this file states which atlas this one fills.

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
    atlas="ObjectAtlas",
    tileset="resources/textures/objects-new.tileset.yaml",
    script="generate_object_atlas_new.py",
    # The version 2 object format lays out variant family columns: a base
    # sprite's `-V1` .. `-VN` stack directly beneath it in the base's own
    # column, inheriting its geometry. The C++ loader derives the same tiles
    # from the same rule, so the sheet, the generated manifest, the
    # placeholder art and the loader agree without anything being written
    # twice (#1053). No shipped object set declares a family yet - the
    # capability lands before the artwork that will use it.
    variant_families=True,
)

if __name__ == "__main__":
    sys.exit(run(SPEC))
