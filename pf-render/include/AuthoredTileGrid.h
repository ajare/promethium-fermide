#pragma once

#include <string>

#include "TileArtwork.h"

#include <yaml-cpp/yaml.h>

// The authored tile grid the grid-based atlas formats share (#1014, #1017).
//
// A tile is placed in a cell of the uniform grid the artist laid the sheet
// out on, and its content rectangle is given inside that cell in image
// pixels. The grid is the artist's layout, not the World cell: the renderer
// stretches a resolved region into whatever destination it is drawn into,
// so the grid is never compared with the cell. All that is required is that
// whole authored cells cover the image with no remainder, because a cell
// coordinate that leaves a remainder resolves to nothing (#1006).
//
// `declaresRenderSemantics` is the version 2 clause: the file authors an
// explicit render offset and drawn size per tile. A version 1 grid authors
// neither, and its drawn size is whatever its content rectangle measures
// over the authored grid.
//
// `tileNoun` names the kind in a refusal - "object sprite", "sector
// surface" - so the atlas loaders report in their own vocabulary while
// resolving the grid once.
struct AuthoredTileGrid
{
    int width{}, height{};
    int tileWidth{}, tileHeight{};
    TileArtworkMap tiles;
};

AuthoredTileGrid loadAuthoredTileGrid(YAML::Node const& root,
    bool declaresRenderSemantics, std::string const& tileNoun);
