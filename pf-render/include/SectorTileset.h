#pragma once
#include <filesystem>
#include <map>
#include <string>
#include "WorldDrawList.h"

// The tilesets' installed-artwork handle, from the shared drawing vocabulary.
// The renderer never dereferences it: it only separates "an atlas is
// installed" from "nothing is", which is what makes a missing atlas draw
// magenta instead of drawing nothing (#1003).
using pf::render::TextureId;

// Pixel regions are explicit: the source artwork is not a uniform grid.
struct SectorTileRegion { int x, y, width, height; };
struct SectorTileset
{
    std::filesystem::path image;
    int width{}, height{};
    std::map<std::string, SectorTileRegion> surfaces;
    std::map<std::string, SectorTileRegion> boundaries;
    static SectorTileset load(std::filesystem::path const& path);
};
void setSectorTileset(SectorTileset tileset, TextureId texture);
void clearSectorTileset();
// The tileset helpers cross to the command stream on the shared drawing
// types (#997), so a caller hands them pf::render::Vec2 and a packed colour.
bool drawSectorTileSurface(std::string const& kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom = 1.0f);
bool drawSectorTileBoundary(char const* kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum);

// No ImDrawList-taking form: a check that wants these on a live ImGui draw
// list records a command vector and replays it from the headless harness
// (#1000). Nothing ImGui is named here: the tileset crossing used to carry
// an ImGui texture id, and the shared renderer has no reason to know that
// type exists (#1003).
