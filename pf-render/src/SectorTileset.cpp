#include "SectorTileset.h"
#include "AuthoredTileGrid.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
SectorTileset active;
TextureId textureId{};

// The ten surface-bearing Sector types. Each fills its own full-cell tile;
// Background and Facade are authored flat colour by domain definition and
// so are not in the set (#1017).
constexpr std::array<char const*, 10> kSectorSurfaces{ "room", "corridor",
    "ladder", "lift", "shuttle", "stairwell", "staircase", "airlock",
    "chamber", "dumbwaiter" };

// `-V` followed by digits and nothing else: exactly the shape the variant
// derivation produces. A sprite authored under one of those names would
// collide with whatever the family rule derives, and which of the two won
// would come down to map order, so the name is refused before anything is
// derived (#1038).
bool isDerivedVariantName(std::string const& name)
{
    auto const mark = name.rfind("-V");
    if (mark == std::string::npos || mark + 2 >= name.size()) return false;
    for (auto it = name.begin() + mark + 2; it != name.end(); ++it)
        if (!std::isdigit(static_cast<unsigned char>(*it))) return false;
    return true;
}

// A version 2 sector tileset authors the same grid the object atlases use:
// a cell in the artist's own layout, a content rectangle inside it, and
// the render semantics per tile (#1014). What differs is what the set is
// for. Every tile in it is a surface; the boundary strips are dropped, so
// every Sector edge falls back to the primitive line the renderer already
// emits. A file that still declares a `boundaries` map is refused rather
// than quietly ignored, which is what stops the dropped artwork coming
// back by accident.
//
// A surface is one full World cell of artwork repeated once per cell across
// the Sector's area. A tile drawn at any other size, or offset off its
// cell's anchor, would repeat at a geometry the per-cell repeat rule cannot
// express, so the loader refuses it rather than drawing something that
// contradicts its own repeat.
//
// A variant family is derived here rather than read. Each base sprite's
// `variants: N` yields `-V1` .. `-VN` in the base's own column, one row
// below the last, each inheriting the base's content rectangle, render
// offset, drawn size and tintable flag. The author states the count and the
// grid does the rest, so a variant's name and its position cannot disagree
// with the family they belong to (#1038).
SectorTileset loadVersion2SectorTileset(std::filesystem::path const& path,
    YAML::Node const& root)
{
    if (root["boundaries"].IsDefined())
        throw std::runtime_error("A version 2 sector tileset declares no boundary "
            "strips; every Sector edge draws the primitive line ('"
            + path.filename().string() + "')");
    // `variants` is a per-sprite key. Written on the file it would read as a
    // set-wide count with no base to descend from, which is not what the key
    // means, so it is refused rather than given a meaning nobody wrote.
    if (root["variants"].IsDefined())
        throw std::runtime_error("`variants` is declared on a base sprite, not on "
            "the file; each family descends from the sprite that stated it ('"
            + path.filename().string() + "')");
    SectorTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    result.family = SectorTilesetFamily::New;
    auto const grid = loadAuthoredTileGrid(root, true, "sector surface",
        AuthoredVariantFamilies::Derived);
    result.width = grid.width;
    result.height = grid.height;
    for (auto const& [name, tile] : grid.tiles)
        if (isDerivedVariantName(name))
            throw std::runtime_error("Sector surface '" + name + "' uses a name the "
                "variant derivation reserves; a family's variants are derived from "
                "its base and are not authored beside it");
    for (auto const& [name, artwork] : grid.tiles) {
        if (artwork.offset.x != 0.0f || artwork.offset.y != 0.0f)
            throw std::runtime_error("Sector surface '" + name
                + "' must be drawn at its cell's anchor, because the repeat is "
                "per World cell");
        if (artwork.drawnSize.x != 1.0f || artwork.drawnSize.y != 1.0f)
            throw std::runtime_error("Sector surface '" + name
                + "' must be drawn at one full World cell, because the repeat "
                "is per World cell");
        result.tiles.emplace(name, artwork);
        result.surfaces.emplace(name, artwork.region);
        auto const declared = grid.variantCounts.find(name);
        if (declared == grid.variantCounts.end()) continue;
        // One row down the sheet is one authored tile height down, and the
        // base's region is already resolved into image pixels, so a variant
        // is the base's own rectangle shifted by that much. Everything else
        // - the content it cuts, the offset it draws at, the size it draws
        // at, whether it takes a tint - is the base's, unchanged.
        for (int variant = 1; variant <= declared->second; ++variant) {
            auto derived = artwork;
            derived.region.y += variant * grid.tileHeight;
            auto const variantName = name + "-V" + std::to_string(variant);
            // The pre-pass above already refuses any authored name in this
            // shape, so this is the backstop rather than the guard: it keeps a
            // derivation that somehow collided from being silently swallowed,
            // which is how a tile ends up drawing as something else.
            if (!result.tiles.emplace(variantName, derived).second)
                throw std::runtime_error("Sector surface '" + variantName
                    + "' is derived from '" + name + "' and cannot also be authored "
                    "beside the family it belongs to");
            result.surfaces.emplace(variantName, derived.region);
        }
        result.variantCounts.emplace(name, declared->second);
    }
    for (auto const* name : kSectorSurfaces)
        if (!result.surfaces.count(name))
            throw std::runtime_error("Missing sector surface: " + std::string(name));
    return result;
}

// Draws one strip of a tile region into a screen rectangle. The four
// fractions say which part of the tile reaches the screen: `u0`/`u1` run
// left to right across the tile, `v0`/`v1` top to bottom. A whole tile is
// 0..1 on both axes; a crop is a sub-range drawn at the same pixels per
// artwork texel the whole tile would have had, which is what keeps a crop a
// crop and not a squash.
void image(WorldDrawList* list, SectorTileRegion const& r, Vec2 a, Vec2 b,
    float u0, float u1, float v0, float v1, PackedColourValue tint)
{
    // Half-texel inset avoids sampling adjacent draft artwork with linear filtering.
    Vec2 uv0{(r.x + 0.5f + (r.width - 1) * u0) / active.width,
        (r.y + 0.5f + (r.height - 1) * v0) / active.height};
    Vec2 uv1{(r.x + 0.5f + (r.width - 1) * u1) / active.width,
        (r.y + 0.5f + (r.height - 1) * v1) / active.height};
    list->AddImage(WorldDrawList::Texture::SectorAtlas, a, b, uv0, uv1, tint);
}

// How many whole World cells a span covers, counted from where it starts.
// The tolerance is not slack. The screen coordinates the span was built
// from carry float round-off of their own, so a Sector four cells wide can
// read as 4.0000004 cells across, and counted without the tolerance that
// fraction becomes a fifth column a hundredth of a pixel wide. A repeat
// that walked the area could hide such a sliver; a loop that resolves a
// tile per cell cannot, because the phantom cell is a cell it has to
// answer for.
int cellsCovered(float span, float cell)
{
    if (cell <= 0.0f || span <= 0.0f) return 0;
    return std::max(0, static_cast<int>(std::ceil(span / cell - 1e-4f)));
}

// Puts one resolved cell on the screen. A name the installed atlas does
// not carry is the missing-tile magenta, and it is magenta for that cell
// alone: the resolver owns the question of which tile, so it owns what a
// wrong answer looks like. A Sector whose base tile is genuinely absent
// still fills its whole area with magenta, one cell at a time, which is
// what it looked like when the whole area was filled at once.
void drawSurfaceCell(WorldDrawList* list, std::string const& kind, Vec2 a,
    Vec2 b, float u0, float u1, float v0, float v1, PackedColourValue tint)
{
    auto const found = active.surfaces.find(kind);
    if (found == active.surfaces.end() || active.width <= 0 || active.height <= 0) {
        list->AddRectFilled(a, b, packColour(255, 0, 255, 255));
        return;
    }
    image(list, found->second, a, b, u0, u1, v0, v1, tint);
}
}

SectorTileset SectorTileset::load(std::filesystem::path const& path)
{
    auto root = YAML::LoadFile(path.string());
    int const version = root["version"].as<int>();
    // Version 1 is the draft's irregular-rectangle format: explicit regions
    // for seven surfaces and four boundary strips. Version 2 is the authored
    // grid, carries no boundary artwork at all, and names every one of the
    // ten surface-bearing Sector types (#1017).
    if (version == 2) return loadVersion2SectorTileset(path, root);
    if (version != 1)
        throw std::runtime_error("Unsupported sector tileset version");
    // The version 1 format has no variant families: its surfaces are bare
    // rectangles with no cell to descend from, so a `variants` key here is a
    // family nothing derives. Refused rather than ignored, because an ignored
    // declaration reads as one that worked (#1038).
    if (root["variants"].IsDefined())
        throw std::runtime_error("A version 1 sector tileset has no variant families; "
            "`variants` is a version 2 base-sprite key ('" + path.filename().string()
            + "')");
    SectorTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    // The draft family. Stated rather than left to the default so the two
    // loaders each say which family they produce (#1027).
    result.family = SectorTilesetFamily::Old;
    result.width = root["size"][0].as<int>();
    result.height = root["size"][1].as<int>();
    if (result.width <= 0 || result.height <= 0)
        throw std::runtime_error("Invalid sector atlas dimensions");
    auto read = [&](char const* key, auto& destination) {
        if (!root[key].IsMap()) throw std::runtime_error("Missing tileset region map");
        for (auto const& entry : root[key]) {
            auto const v = entry.second;
            if (!v.IsSequence() || v.size() != 4)
                throw std::runtime_error("Tileset regions require [x, y, width, height]");
            SectorTileRegion r{v[0].as<int>(), v[1].as<int>(), v[2].as<int>(), v[3].as<int>()};
            if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
                r.width > result.width || r.height > result.height ||
                r.x > result.width - r.width || r.y > result.height - r.height)
                throw std::runtime_error("Sector atlas region is out of bounds");
            destination.emplace(entry.first.as<std::string>(), r);
        }
    };
    read("surfaces", result.surfaces);
    read("boundaries", result.boundaries);
    for (auto const& [name, region] : result.surfaces)
        result.tiles.emplace(name, TileArtwork{ region, false, { 0.0f, 0.0f }, { 1.0f, 1.0f } });
    for (auto const& [name, region] : result.boundaries)
        result.tiles.emplace("boundary-" + name,
            TileArtwork{ region, false, { 0.0f, 0.0f }, { 1.0f, 1.0f } });
    for (auto name : {"corridor", "room", "ladder", "lift", "shuttle", "stairwell", "staircase"})
        if (!result.surfaces.count(name)) throw std::runtime_error("Missing sector surface: " + std::string(name));
    for (auto name : {"floor", "ceiling", "left", "right"})
        if (!result.boundaries.count(name)) throw std::runtime_error("Missing sector boundary: " + std::string(name));
    return result;
}

void setSectorTileset(SectorTileset tileset, TextureId texture)
{
    active = std::move(tileset);
    textureId = texture;
}
void clearSectorTileset() { textureId = {}; active = {}; }

bool hasSectorTileSurface(std::string const& kind)
{
    return bool(textureId) && active.surfaces.count(kind) > 0;
}

bool sectorTilesetIsNewFamily()
{
    // An uninstalled atlas is not the new family. `clearSectorTileset()`
    // resets `active`, so the family resets with it and nothing survives a
    // teardown as a stale promise that own art is available.
    return bool(textureId) && active.family == SectorTilesetFamily::New;
}

int sectorSurfaceVariantCount(std::string const& kind)
{
    // Guarded on the install the same way the surface lookup is: with no
    // atlas behind the handle there is no declared count to report. The
    // version 1 family declares no families either, and its loader leaves
    // the map empty, so both answer 0 for the same reason - no author ever
    // stated a count.
    if (!textureId) return 0;
    return active.variantCount(kind);
}

int SectorTileset::variantCount(std::string const& kind) const
{
    auto const found = variantCounts.find(kind);
    return found == variantCounts.end() ? 0 : found->second;
}

bool drawSectorTileSurfacePerCell(WorldDrawList* list, Vec2 minimum,
    Vec2 maximum, PackedColourValue tint, float zoom,
    SectorSurfaceTileResolver const& resolveTile)
{
    if (!textureId) return false;
    // One tile per 1x1 world unit, magnified to the World cell's screen size
    // at the current zoom (the two cell sizes below). Partial levels are
    // cropped, never stretched to a Sector's height.
    //
    // The grid is anchored on the bottom of the drawn area, which for a
    // Sector is its floor line: the tile's own bottom edge lands there and
    // further rows stack upward. A Sector shorter than a full cell - a 0.7
    // Corridor, a 0.9 Room - keeps the end of the tile it was drawn beside,
    // the floor end, at one artwork pixel to one screen pixel, and loses the
    // top of the tile that would have stood above its ceiling. Anchoring on
    // the top instead cuts the floor off every short Sector and leaves the
    // tile's ceiling band sitting on the floor it is supposed to be above.
    float const cellWidth = CORE_CELL_WIDTH_PIXELS * zoom;
    float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * zoom;
    if (cellWidth <= 0 || cellHeight <= 0) return true;
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    float const top = std::max(minimum.y, clipMin.y);
    float const right = std::min(maximum.x, clipMax.x);
    if (top >= maximum.y || right <= minimum.x) return true;
    // The grid is counted from the area's own left edge and floor line, not
    // from the clip, and the resolver is asked in that frame. A Sector
    // scrolled half out of view therefore still resolves its own columns
    // and levels: the clip decides only which of them are worth emitting,
    // never which tile a cell wears or where its edges fall.
    int const columns = cellsCovered(maximum.x - minimum.x, cellWidth);
    int const rows = cellsCovered(maximum.y - minimum.y, cellHeight);
    int const firstColumn = std::max(0, static_cast<int>(
        (clipMin.x - minimum.x) / cellWidth));
    // Rows are counted up from the floor line, so the clip cannot move the
    // anchor; it only decides which rows are worth emitting. Everything
    // above `top` is left out, and so is every row that lies wholly below
    // the clip's bottom edge - but not a row the clip cuts through. That
    // last row is partly on screen and has to be emitted, at its full cell
    // rect and sampling its whole tile, for the scissor to crop it. Rounding
    // the distance up (`ceil`, which is what this line did until #1048)
    // rounds past exactly that row: with the clip's bottom edge one pixel
    // above a Sector's floor line, the whole bottom row of that Sector -
    // 319 of its 320 pixels visible - went undrawn. Truncation is the same
    // shape `firstColumn` two lines above already uses, and it stays exact at
    // the boundary: a clip bottom landing exactly on a row boundary leaves
    // that boundary's row below the cut, where it belongs, because the row
    // whose bottom edge *is* the cut lies under the clip rather than above
    // it.
    int const firstRow = std::max(0, static_cast<int>(
        (maximum.y - std::min(maximum.y, clipMax.y)) / cellHeight));
    for (int row = firstRow; row < rows; ++row) {
        float const rowBottom = maximum.y - float(row) * cellHeight;
        if (rowBottom <= top) break;
        float const rowTop = std::max(rowBottom - cellHeight, top);
        // Whatever of the tile stands above this row's top edge never reaches
        // the screen, so sampling starts that far down the tile.
        float const v0 = 1.0f - (rowBottom - rowTop) / cellHeight;
        for (int column = firstColumn; column < columns; ++column) {
            float const x = minimum.x + float(column) * cellWidth;
            if (x >= right) break;
            float const colRight = std::min(x + cellWidth, right);
            drawSurfaceCell(list,
                resolveTile(static_cast<std::uint32_t>(column),
                    static_cast<std::uint32_t>(row)),
                { x, rowTop }, { colRight, rowBottom }, 0.0f,
                (colRight - x) / cellWidth, v0, 1.0f, tint);
        }
    }
    return true;
}

bool drawSectorTileSurface(std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom)
{
    // The per-cell loop with one name answered for every cell (#1039). A
    // Sector with no per-cell artwork resolves to exactly this, and it is
    // what a caller asks for when it has one tile and no wish to say which
    // cell wears it.
    return drawSectorTileSurfacePerCell(list, minimum, maximum, tint, zoom,
        [&](std::uint32_t, std::uint32_t) { return kind; });
}

bool drawSectorTilePreview(std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint)
{
    // No atlas, or no tile under that name: nothing to frame, and the caller
    // is told so it can leave the swatch off rather than show an empty one.
    if (!textureId || !list) return false;
    auto const found = active.surfaces.find(kind);
    if (found == active.surfaces.end() || active.width <= 0 || active.height <= 0)
        return false;
    // The rect may be handed over either way round; frame the tile inside
    // whichever corners it actually spans.
    auto const left = std::min(minimum.x, maximum.x);
    auto const right = std::max(minimum.x, maximum.x);
    auto const top = std::min(minimum.y, maximum.y);
    auto const bottom = std::max(minimum.y, maximum.y);
    // A rect with no area has nothing to frame, but the tile was found, so
    // this is a yes with no command rather than a missing tile.
    if (left == right || top == bottom) return true;
    // The whole tile, both fraction pairs running end to end. Nothing is
    // cropped and nothing is repeated: whatever the rect's aspect, the tile's
    // top-left lands on the rect's top-left and its bottom-right on the
    // rect's bottom-right. The authored render offset and drawn size are
    // World-space facts about how a Sector wears the tile and say nothing
    // about how a swatch frames it, so they are not consulted here.
    image(list, found->second, { left, top }, { right, bottom }, 0.0f, 1.0f,
        0.0f, 1.0f, tint);
    return true;
}

bool drawSectorTileBoundary(char const* kind, WorldDrawList* list, Vec2 a, Vec2 b)
{
    if (!textureId) return false;
    auto const found = active.boundaries.find(kind);
    if (found == active.boundaries.end() || active.width <= 0 || active.height <= 0) return false;
    image(list, found->second, a, b, 0.0f, 1.0f, 0.0f, 1.0f, White);
    return true;
}
