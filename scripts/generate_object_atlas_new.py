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
)

if __name__ == "__main__":
    sys.exit(run(SPEC))
