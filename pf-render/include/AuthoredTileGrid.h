#pragma once

#include <map>
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
//
// Whether a base sprite may declare a variant family is the caller's call,
// because it is not a fact about the grid. A `variants: N` key promises N
// more tiles stacked in the base's own column directly beneath it, and only
// a format that lays those columns out can keep that promise. Where the
// format has no column to grow, the key is refused rather than ignored: an
// ignored declaration is a family an author believes exists and nothing
// derives (#1038).
enum class AuthoredVariantFamilies
{
    // The key is not part of this format. A sprite carrying it is refused.
    Refused,
    // The key is legal on a base sprite, and the declared count must fit
    // the sheet below the sprite it was written on.
    Derived
};

struct AuthoredTileGrid
{
    int width{}, height{};
    int tileWidth{}, tileHeight{};
    TileArtworkMap tiles;
    // How many variants each base sprite declared, keyed by that sprite's
    // own name. Only filled when the caller derives variant families; the
    // count is the author's statement, not something read back off the
    // derived tiles, so a caller can tell "declared none" from "never had
    // the option".
    std::map<std::string, int> variantCounts;
};

AuthoredTileGrid loadAuthoredTileGrid(YAML::Node const& root,
    bool declaresRenderSemantics, std::string const& tileNoun,
    AuthoredVariantFamilies variantFamilies
        = AuthoredVariantFamilies::Refused);
