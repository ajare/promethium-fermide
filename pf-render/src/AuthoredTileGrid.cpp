#include "AuthoredTileGrid.h"

#include <cmath>
#include <stdexcept>

namespace {
// Reads one required pair of World-unit numbers out of a tileset node. A
// field the file never wrote, or one that is not a finite number, is a
// refusal rather than a default: an offset that quietly became zero would
// put artwork where no author said it goes.
pf::render::Vec2 requirePair(YAML::Node const& node, std::string const& tile,
    std::string const& tileNoun, char const* field)
{
    auto const value = node[field];
    if (!value.IsSequence() || value.size() != 2)
        throw std::runtime_error(tileNoun + " '" + tile + "' is missing its "
            + std::string(field) + " pair");
    double first{}, second{};
    try { first = value[0].as<double>(); second = value[1].as<double>(); }
    catch (YAML::Exception const&)
    {
        throw std::runtime_error(tileNoun + " '" + tile + "' " + field
            + " must be two numbers");
    }
    if (!std::isfinite(first) || !std::isfinite(second))
        throw std::runtime_error(tileNoun + " '" + tile + "' " + field
            + " must be finite");
    return { static_cast<float>(first), static_cast<float>(second) };
}
}

AuthoredTileGrid loadAuthoredTileGrid(YAML::Node const& root,
    bool declaresRenderSemantics, std::string const& tileNoun)
{
    AuthoredTileGrid grid;
    grid.width = root["size"][0].as<int>();
    grid.height = root["size"][1].as<int>();
    grid.tileWidth = root["tile_size"][0].as<int>();
    grid.tileHeight = root["tile_size"][1].as<int>();
    int const tw = grid.tileWidth, th = grid.tileHeight;
    // The authored tile grid is how the artist laid the image out; the World
    // cell is how large a World unit is on screen. They are not the same fact,
    // and the renderer stretches a resolved region into whatever destination it
    // is drawn into, so the grid is never compared with the cell. All that is
    // required is that whole authored cells cover the image with no remainder,
    // because a cell coordinate that leaves a remainder resolves to nothing
    // (#1006).
    if (tw <= 0 || th <= 0 || grid.width <= 0 || grid.height <= 0 ||
        grid.width % tw || grid.height % th)
        throw std::runtime_error("An authored tile grid must cover the atlas "
            "image with no remainder");
    if (!root["sprites"].IsMap())
        throw std::runtime_error("Missing " + tileNoun + " definitions");
    for (auto const& item : root["sprites"]) {
        auto const v = item.second;
        if (!v["cell"].IsSequence() || v["cell"].size() != 2 ||
            !v["content"].IsSequence() || v["content"].size() != 4)
            throw std::runtime_error("Invalid " + tileNoun + " rectangle");
        auto const c = v["content"];
        int const column = v["cell"][0].as<int>(), row = v["cell"][1].as<int>();
        SectorTileRegion r{c[0].as<int>(), c[1].as<int>(), c[2].as<int>(), c[3].as<int>()};
        if (column < 0 || row < 0 || column >= grid.width / tw || row >= grid.height / th ||
            r.x < 0 || r.x >= tw || r.y < 0 || r.width <= 0 || r.height <= 0 || r.height > th ||
            r.width > grid.width || column * tw + r.x > grid.width - r.width || r.y > th - r.height)
            throw std::runtime_error(tileNoun + " is outside its cell");
        r.x += column * tw; r.y += row * th;
        auto const name = item.first.as<std::string>();
        TileArtwork artwork{ r, v["tintable"].as<bool>(), { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        if (declaresRenderSemantics)
        {
            artwork.offset = requirePair(v, name, tileNoun, "offset");
            artwork.drawnSize = requirePair(v, name, tileNoun, "drawn_size");
            if (artwork.drawnSize.x <= 0.0f || artwork.drawnSize.y <= 0.0f)
                throw std::runtime_error(tileNoun + " '" + name
                    + "' drawn size must be positive");
            // One World unit of authored art is one authored tile, so a
            // drawn size larger than the image could never be drawn from it.
            if (artwork.drawnSize.x * tw > grid.width + 1e-4
                || artwork.drawnSize.y * th > grid.height + 1e-4)
                throw std::runtime_error(tileNoun + " '" + name
                    + "' drawn size does not fit the atlas image");
        }
        else
        {
            // A version 1 tileset authors no render semantics. The identity
            // is the format's own: no offset, and a drawn size of whatever
            // the content rectangle measures in World units over the
            // authored grid.
            artwork.drawnSize = { float(r.width) / float(tw), float(r.height) / float(th) };
        }
        if (!grid.tiles.emplace(name, artwork).second)
            throw std::runtime_error("Duplicate " + tileNoun);
    }
    return grid;
}
