#include "SectorTileset.h"
#include "core/Defines.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pf::application::render {

namespace {
SectorTileset active;
void image(WorldDrawList* list, SectorTileRegion const& r, Vec2 a, Vec2 b,
    float fractionX, float fractionY, PackedColourValue tint)
{
    // Half-texel inset avoids sampling adjacent draft artwork with linear filtering.
    Vec2 uv0{(r.x + 0.5f) / active.width, (r.y + 0.5f) / active.height};
    Vec2 uv1{(r.x + 0.5f + (r.width - 1) * fractionX) / active.width,
        (r.y + 0.5f + (r.height - 1) * fractionY) / active.height};
    list->AddImage(WorldDrawList::Texture::SectorAtlas, a, b, uv0, uv1, tint);
}
}

void setSectorTileset(SectorTileset tileset)
{
    active = std::move(tileset);
}
void clearSectorTileset() { active = {}; }

bool drawSectorTileSurface(std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint)
{
    if (active.width <= 0 || active.height <= 0) return false;
    auto const& region = active.surfaces.at(kind);
    // One world unit is 64 screen pixels wide and 160 high. Crop partial
    // levels instead of stretching a complete tile to a Corridor's height.
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    int const x0 = std::max(0, static_cast<int>((clipMin.x - minimum.x) / CORE_CELL_WIDTH_PIXELS));
    int const y0 = std::max(0, static_cast<int>((clipMin.y - minimum.y) / CORE_LEVEL_HEIGHT_PIXELS));
    for (float y = minimum.y + y0 * CORE_LEVEL_HEIGHT_PIXELS; y < std::min(maximum.y, clipMax.y); y += CORE_LEVEL_HEIGHT_PIXELS)
        for (float x = minimum.x + x0 * CORE_CELL_WIDTH_PIXELS; x < std::min(maximum.x, clipMax.x); x += CORE_CELL_WIDTH_PIXELS) {
            Vec2 end{std::min(x + CORE_CELL_WIDTH_PIXELS, maximum.x), std::min(y + CORE_LEVEL_HEIGHT_PIXELS, maximum.y)};
            image(list, region, {x, y}, end, (end.x - x) / CORE_CELL_WIDTH_PIXELS,
                (end.y - y) / CORE_LEVEL_HEIGHT_PIXELS, tint);
        }
    return true;
}

bool drawSectorTileBoundary(char const* kind, WorldDrawList* list, Vec2 a, Vec2 b)
{
    if (active.width <= 0 || active.height <= 0) return false;
    image(list, active.boundaries.at(kind), a, b, 1, 1, White);
    return true;
}

} // namespace pf::application::render
