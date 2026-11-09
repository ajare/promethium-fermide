#include "SectorTileset.h"
#include "AuthoredTileGrid.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
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
    // screen size at the current zoom (the macros below). Crop partial levels
    // instead of stretching a complete tile to a Corridor's height.
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
    if (!textureId) return false;
    auto const found = active.boundaries.find(kind);
    if (found == active.boundaries.end() || active.width <= 0 || active.height <= 0) return false;
    image(list, found->second, a, b, 1, 1, White);
    return true;
}
