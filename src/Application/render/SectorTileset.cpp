#include "SectorTileset.h"
#include "core/Defines.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pf::application::render {

namespace {
SectorTileset active;
bool enabled{};
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
    enabled = true;
}
void clearSectorTileset() { active = {}; enabled = false; }

bool drawSectorTileSurface(std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom)
{
    if (!enabled) return false;
    auto const found = active.surfaces.find(kind);
    if (found == active.surfaces.end() || active.width <= 0 || active.height <= 0) {
        list->AddRectFilled(minimum, maximum, packColour(255, 0, 255));
        return true;
    }
    auto const& region = found->second;
    // One world unit is 64 screen pixels wide and 160 high. Crop partial
    // levels instead of stretching a complete tile to a Corridor's height.
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    float const cellWidth = CORE_CELL_WIDTH_PIXELS * zoom;
    float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * zoom;
    if (cellWidth <= 0 || cellHeight <= 0) return true;
    int const x0 = std::max(0, static_cast<int>((clipMin.x - minimum.x) / cellWidth));
    int const y0 = std::max(0, static_cast<int>((clipMin.y - minimum.y) / cellHeight));
    for (float y = minimum.y + y0 * cellHeight; y < std::min(maximum.y, clipMax.y); y += cellHeight)
        for (float x = minimum.x + x0 * cellWidth; x < std::min(maximum.x, clipMax.x); x += cellWidth) {
            Vec2 end{std::min(x + cellWidth, maximum.x), std::min(y + cellHeight, maximum.y)};
            image(list, region, {x, y}, end, (end.x - x) / cellWidth,
                (end.y - y) / cellHeight, tint);
        }
    return true;
}

bool drawSectorTileBoundary(char const* kind, WorldDrawList* list, Vec2 a, Vec2 b)
{
    if (active.width <= 0 || active.height <= 0) return false;
    auto const found = active.boundaries.find(kind);
    if (found == active.boundaries.end()) return false;
    image(list, found->second, a, b, 1, 1, White);
    return true;
}

} // namespace pf::application::render
