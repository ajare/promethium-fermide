#pragma once

#include <map>
#include <string>

#include "DrawTypes.h"

// One tile's artwork as the shared renderer holds it after install (#1014).
//
// Two authorities meet here and neither one derives the other:
//
//   * The pixel region is the runtime contract. It comes from the atlas's
//     Willpower `ImageSet` resource, exactly as ADR 0010 requires.
//   * The two render semantics - the render offset and the drawn size - are
//     authored per tile in the tileset definition that the `ImageSet` names.
//     They are read from there. They are never derived from a tile's name,
//     and the drawn size is never taken from the physical extent of whatever
//     object the tile is drawn for.
//
// What a tile looks like is settled by its pixels and nothing else. No colour
// is multiplied into a tile on the way to the screen: the artwork the sheet
// carries is the artwork that draws, so a picture that needs to be darker,
// lighter or another hue is a different picture rather than the same one
// drawn differently.
//
// That separation is what lets a Walkway keep a deliberately zero-height
// physical shape - it must never obstruct pathing - while its artwork hangs
// below the floor line of the cell it was authored in. CONTEXT.md keeps the
// same line: Support elevation is a physical fact and is explicitly not a
// render offset.
struct SectorTileRegion { int x, y, width, height; };

struct TileArtwork
{
    SectorTileRegion region{ 0, 0, 0, 0 };
    // Render offset in World units from the tile's anchor: +x right, +y up.
    pf::render::Vec2 offset{ 0.0f, 0.0f };
    // Drawn size in World units. An artwork's visual extent is its own and
    // says nothing about what the tile physically blocks.
    pf::render::Vec2 drawnSize{ 0.0f, 0.0f };
};

using TileArtworkMap = std::map<std::string, TileArtwork>;

// The pixel region one `ImageSet` entry declares. Kept apart from
// TileArtwork so the install seam names both authorities explicitly.
struct AtlasPixelRegion { int x{ 0 }, y{ 0 }, width{ 0 }, height{ 0 }; };
using AtlasPixelRegions = std::map<std::string, AtlasPixelRegion>;

// The shared core of the atlas install path (#1014): pairs the pixel regions
// an `ImageSet` declares with the render semantics the authored tileset
// declared under the same tile name.
//
// A region that escapes the atlas image is refused, and so is a tile the
// `ImageSet` declares that the tileset never authored - the renderer would
// be left with no declared visual extent for it, which is the drift this
// split exists to prevent.
TileArtworkMap installAtlasTiles(TileArtworkMap const& authored,
    AtlasPixelRegions const& regions, int imageWidth, int imageHeight,
    std::string const& atlasName);
