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
//
// Where it is derived, the grid derives it. Naming the `-V1` .. `-VN` tiles
// and stepping them down the base's column is the grid's arithmetic, not the
// atlas's, so every format that takes the capability gets the same rule and
// the same refusals rather than one copy per loader (#1053).
enum class AuthoredVariantFamilies
{
    // The key is not part of this format. A sprite carrying it is refused.
    Refused,
    // The key is legal on a base sprite. The declared count must fit the
    // sheet below the sprite it was written on, and the `-V1` .. `-VN` it
    // describes are derived into `tiles` beside the authored sprites.
    Derived
};

struct AuthoredTileGrid
{
    int width{}, height{};
    int tileWidth{}, tileHeight{};
    // Every tile the sheet holds. When the caller derives variant families
    // this is the authored sprites plus the `-V1` .. `-VN` each of them
    // declared, so a caller never has to ask which of the two a name is.
    TileArtworkMap tiles;
    // How many variants each base sprite declared, keyed by that sprite's
    // own name. Only filled when the caller derives variant families; the
    // count is the author's statement, not something read back off the
    // derived tiles, so a caller can tell "declared none" from "never had
    // the option". A derived `-Vn` tile is not a base and holds no count of
    // its own.
    std::map<std::string, int> variantCounts;
};

// Where the sprites are read from is a parameter rather than a constant
// because the same grid authors more than one kind of tile in one file. A
// version 2 sector tileset keeps its surfaces under `sprites` and its
// doorframes under `overlays`, and both are cells on the same sheet with
// the same content rule and the same refusals - which is exactly why one
// loader reads them both rather than a second one written beside it
// (#1054).
//
// The key is required here: a defined Null at `spritesKey` is refused as
// "missing", and anything that is neither a Null nor a map is refused as
// the wrong shape. A caller that means the key to be optional - a version
// 2 sector file may leave `overlays` out entirely, or stub it with no
// entries - has to settle that shape question before calling, because the
// grid has no notion of an optional map (#1066).
AuthoredTileGrid loadAuthoredTileGrid(YAML::Node const& root,
    bool declaresRenderSemantics, std::string const& tileNoun,
    AuthoredVariantFamilies variantFamilies
        = AuthoredVariantFamilies::Refused,
    std::string const& spritesKey = "sprites");
