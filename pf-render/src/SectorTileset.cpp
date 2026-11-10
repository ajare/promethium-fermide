#include "SectorTileset.h"
#include "AuthoredTileGrid.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
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
SectorTileset loadVersion2SectorTileset(std::filesystem::path const& path,
    YAML::Node const& root)
{
    if (root["boundaries"].IsDefined())
        throw std::runtime_error("A version 2 sector tileset declares no boundary "
            "strips; every Sector edge draws the primitive line ('"
            + path.filename().string() + "')");
    SectorTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    result.family = SectorTilesetFamily::New;
    auto const grid = loadAuthoredTileGrid(root, true, "sector surface");
    result.width = grid.width;
    result.height = grid.height;
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

bool drawSectorTileSurface(std::string const& kind, WorldDrawList* list,
    Vec2 minimum, Vec2 maximum, PackedColourValue tint, float zoom)
{
    if (!textureId) return false;
    auto const found = active.surfaces.find(kind);
    if (found == active.surfaces.end() || active.width <= 0 || active.height <= 0) {
        list->AddRectFilled(minimum, maximum, packColour(255, 0, 255, 255));
        return true;
    }
    auto const& region = found->second;
    // A surface repeats once per 1x1 world unit, magnified to the World cell's
    // screen size at the current zoom (the macros below). Partial levels are
    // cropped, never stretched to a Sector's height.
    //
    // The repeat is anchored on the bottom of the Sector's area, which is its
    // floor line: the tile's own bottom edge lands there and further rows stack
    // upward. A Sector shorter than a full cell - a 0.7 Corridor, a 0.9 Room -
    // keeps the end of the tile it was drawn beside, the floor end, at one
    // artwork pixel to one screen pixel, and loses the top of the tile that
    // would have stood above its ceiling. Anchoring on the top instead cuts the
    // floor off every short Sector and leaves the tile's ceiling band sitting on
    // the floor it is supposed to be above.
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    float const cellWidth = CORE_CELL_WIDTH_PIXELS * zoom;
    float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * zoom;
    if (cellWidth <= 0 || cellHeight <= 0) return true;
    float const top = std::max(minimum.y, clipMin.y);
    float const right = std::min(maximum.x, clipMax.x);
    if (top >= maximum.y || right <= minimum.x) return true;
    int const x0 = std::max(0, static_cast<int>((clipMin.x - minimum.x) / cellWidth));
    // Rows are counted up from the floor line, so the clip cannot move the
    // anchor; it only decides which rows are worth emitting. Everything below
    // the clip's bottom edge and everything above `top` is left out.
    int const firstRow = std::max(0, static_cast<int>(std::ceil(
        (maximum.y - std::min(maximum.y, clipMax.y)) / cellHeight)));
    for (int row = firstRow; ; ++row) {
        float const rowBottom = maximum.y - float(row) * cellHeight;
        if (rowBottom <= top) break;
        float const rowTop = std::max(rowBottom - cellHeight, top);
        // Whatever of the tile stands above this row's top edge never reaches
        // the screen, so sampling starts that far down the tile.
        float const v0 = 1.0f - (rowBottom - rowTop) / cellHeight;
        for (float x = minimum.x + float(x0) * cellWidth; x < right; x += cellWidth) {
            float const colRight = std::min(x + cellWidth, right);
            image(list, region, {x, rowTop}, {colRight, rowBottom}, 0.0f,
                (colRight - x) / cellWidth, v0, 1.0f, tint);
        }
    }
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
