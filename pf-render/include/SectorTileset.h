#pragma once
#include <filesystem>
#include <map>
#include <string>
#include "imgui/imgui.h"
#include "WorldDrawList.h"

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
void setSectorTileset(SectorTileset tileset, ImTextureID texture);
void clearSectorTileset();
// The tileset helpers cross to the command stream on the shared drawing
// types (#997), so a caller hands them pf::render::Vec2 and a packed colour.
bool drawSectorTileSurface(std::string const& kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom = 1.0f);
bool drawSectorTileBoundary(char const* kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum);

inline bool drawSectorTileSurface(std::string const& kind, ImDrawList* drawList,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint)
{
    WorldDrawList adapter(drawList);
    return drawSectorTileSurface(kind, &adapter, minimum, maximum, tint);
}

inline bool drawSectorTileBoundary(char const* kind, ImDrawList* drawList,
    Vec2 minimum, Vec2 maximum)
{
    WorldDrawList adapter(drawList);
    return drawSectorTileBoundary(kind, &adapter, minimum, maximum);
}
