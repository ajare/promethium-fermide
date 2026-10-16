#pragma once
#include "Application.h"
#include <map>
#include <string>
#include "DrawTypes.h"
#include "WorldDrawList.h"

namespace pf::application::render {

// Pixel regions are explicit: the source artwork is not a uniform grid.
struct SectorTileRegion { int x, y, width, height; };
struct SectorTileset
{
    int width{}, height{};
    std::map<std::string, SectorTileRegion> surfaces;
    std::map<std::string, SectorTileRegion> boundaries;
};
PF_APPLICATION_API void setSectorTileset(SectorTileset tileset);
PF_APPLICATION_API void clearSectorTileset();
bool drawSectorTileSurface(std::string const& kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom = 1.0f);
bool drawSectorTileBoundary(char const* kind, WorldDrawList* drawList,
    Vec2 minimum, Vec2 maximum);

} // namespace pf::application::render
