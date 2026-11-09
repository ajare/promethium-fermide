#include "ObjectTileset.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
namespace {
ObjectTileset active;
TextureId textureId{};
ObjectTileset agents;
TextureId agentTextureId{};
ObjectTileset furniture;
TextureId furnitureTextureId{};

// Reads one required pair of World-unit numbers out of a tileset node. A
// field the file never wrote, or one that is not a finite number, is a
// refusal rather than a default: an offset that quietly became zero would
// put artwork where no author said it goes.
pf::render::Vec2 requirePair(YAML::Node const& node, std::string const& sprite,
    char const* field)
{
    auto const value = node[field];
    if (!value.IsSequence() || value.size() != 2)
        throw std::runtime_error("Object sprite '" + sprite + "' is missing its "
            + std::string(field) + " pair");
    double first{}, second{};
    try { first = value[0].as<double>(); second = value[1].as<double>(); }
    catch (YAML::Exception const&)
    {
        throw std::runtime_error("Object sprite '" + sprite + "' " + field
            + " must be two numbers");
    }
    if (!std::isfinite(first) || !std::isfinite(second))
        throw std::runtime_error("Object sprite '" + sprite + "' " + field
            + " must be finite");
    return { static_cast<float>(first), static_cast<float>(second) };
}
}
ObjectTileset ObjectTileset::load(std::filesystem::path const& path)
{
    auto root = YAML::LoadFile(path.string());
    int const version = root["version"].as<int>();
    // Version 1 declares a region and a tintable flag per tile. Version 2
    // adds the render offset and the drawn size, so a tile's visual extent
    // is authored rather than inferred from whatever object draws it
    // (#1014).
    if (version != 1 && version != 2)
        throw std::runtime_error("Unsupported object tileset version");
    ObjectTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    result.width = root["size"][0].as<int>();
    result.height = root["size"][1].as<int>();
    int const tw = root["tile_size"][0].as<int>();
    int const th = root["tile_size"][1].as<int>();
    // The authored tile grid is how the artist laid the image out; the World
    // cell is how large a World unit is on screen. They are not the same fact,
    // and the renderer stretches a resolved region into whatever destination it
    // is drawn into, so the grid is never compared with the cell. All that is
    // required is that whole authored cells cover the image with no remainder,
    // because a cell coordinate that leaves a remainder resolves to nothing
    // (#1006).
    if (tw <= 0 || th <= 0 || result.width <= 0 || result.height <= 0 ||
        result.width % tw || result.height % th)
        throw std::runtime_error("Object atlas must contain whole authored tiles");
    if (!root["sprites"].IsMap()) throw std::runtime_error("Missing object sprite definitions");
    for (auto const& item : root["sprites"]) {
        auto const v = item.second;
        if (!v["cell"].IsSequence() || v["cell"].size() != 2 ||
            !v["content"].IsSequence() || v["content"].size() != 4)
            throw std::runtime_error("Invalid object sprite rectangle");
        auto const c = v["content"];
        int const column = v["cell"][0].as<int>(), row = v["cell"][1].as<int>();
        SectorTileRegion r{c[0].as<int>(), c[1].as<int>(), c[2].as<int>(), c[3].as<int>()};
        if (column < 0 || row < 0 || column >= result.width / tw || row >= result.height / th ||
            r.x < 0 || r.x >= tw || r.y < 0 || r.width <= 0 || r.height <= 0 || r.height > th ||
            r.width > result.width || column * tw + r.x > result.width - r.width || r.y > th - r.height)
            throw std::runtime_error("Object sprite is outside its cell");
        r.x += column * tw; r.y += row * th;
        auto const name = item.first.as<std::string>();
        TileArtwork artwork{ r, v["tintable"].as<bool>(), { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        if (version >= 2)
        {
            artwork.offset = requirePair(v, name, "offset");
            artwork.drawnSize = requirePair(v, name, "drawn_size");
            if (artwork.drawnSize.x <= 0.0f || artwork.drawnSize.y <= 0.0f)
                throw std::runtime_error("Object sprite '" + name
                    + "' drawn size must be positive");
            // One World unit of authored art is one authored tile, so a
            // drawn size larger than the image could never be drawn from it.
            if (artwork.drawnSize.x * tw > result.width + 1e-4
                || artwork.drawnSize.y * th > result.height + 1e-4)
                throw std::runtime_error("Object sprite '" + name
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
        if (!result.sprites.emplace(name, artwork).second)
            throw std::runtime_error("Duplicate object sprite");
    }
    // The required-tile list is the version 1 object atlas's own contract.
    // A version 2 set is a different authored set with a different shape;
    // which tiles a World requires from it is validated where the World's
    // atlas selection is resolved, not by holding every old set's list.
    if (version == 1)
    {
        for (auto name : {"door", "button-enabled", "button-disabled", "window-clear", "window-frosted",
            "window-tinted", "booth-window-open", "booth-window-closed", "agent", "marker", "platform-lift", "shuttle-car-left",
            "shuttle-car-middle", "shuttle-car-right", "shuttle-car-connector", "lift-car"})
            if (!result.sprites.count(name)) throw std::runtime_error("Missing object sprite: " + std::string(name));
        if (!result.sprites.at("agent").tintable)
            throw std::runtime_error("Agent sprite must support tinting");
    }
    return result;
}
void setObjectTileset(ObjectTileset tileset, TextureId texture)
{
    active = std::move(tileset); textureId = texture;
}
void clearObjectTileset() { textureId = {}; active = {}; }
void setAgentTileset(ObjectTileset tileset, TextureId texture)
{
    agents = std::move(tileset); agentTextureId = texture;
}
void clearAgentTileset() { agents = {}; agentTextureId = {}; }
std::optional<Vec2> agentSpriteSize(char const* name)
{
    auto const found = agents.sprites.find(name);
    if (!agentTextureId || agents.width <= 0 || agents.height <= 0 || found == agents.sprites.end())
        return std::nullopt;
    return Vec2{float(found->second.region.width), float(found->second.region.height)};
}
bool drawAgentSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b, PackedColourValue tint)
{
    if (!agentSpriteSize(name)) return false;
    auto const& r = agents.sprites.at(name).region;
    Vec2 uv0{(r.x + .5f) / agents.width, (r.y + .5f) / agents.height};
    Vec2 uv1{(r.x + r.width - .5f) / agents.width, (r.y + r.height - .5f) / agents.height};
    list->AddImage(WorldDrawList::Texture::AgentAtlas, a, b, uv0, uv1, tint);
    return true;
}
void setFurnitureTileset(ObjectTileset tileset, TextureId texture)
{
    furniture = std::move(tileset); furnitureTextureId = texture;
}
void clearFurnitureTileset() { furniture = {}; furnitureTextureId = {}; }
std::optional<Vec2> furnitureSpriteSize(char const* name)
{
    auto const found = furniture.sprites.find(name);
    if (!furnitureTextureId || furniture.width <= 0 || furniture.height <= 0 || found == furniture.sprites.end())
        return std::nullopt;
    return Vec2{float(found->second.region.width), float(found->second.region.height)};
}
bool drawFurnitureSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b)
{
    if (!furnitureSpriteSize(name)) return false;
    auto const& r = furniture.sprites.at(name).region;
    Vec2 minimum{std::min(a.x, b.x), std::min(a.y, b.y)};
    Vec2 maximum{std::max(a.x, b.x), std::max(a.y, b.y)};
    list->AddImage(WorldDrawList::Texture::FurnitureAtlas, minimum, maximum,
        {(r.x + .5f) / furniture.width, (r.y + .5f) / furniture.height},
        {(r.x + r.width - .5f) / furniture.width, (r.y + r.height - .5f) / furniture.height});
    return true;
}
bool hasObjectTileset() { return textureId != TextureId{}; }
std::optional<Vec2> objectSpriteSize(char const* name)
{
    auto const sprite = active.sprites.find(name);
    if (!textureId || sprite == active.sprites.end()) return std::nullopt;
    return Vec2{static_cast<float>(sprite->second.region.width),
        static_cast<float>(sprite->second.region.height)};
}
std::optional<TileArtwork> objectTileArtwork(std::string const& name)
{
    auto const sprite = active.sprites.find(name);
    if (!textureId || sprite == active.sprites.end()) return std::nullopt;
    return sprite->second;
}
bool drawObjectSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint, Vec2 sourceMin, Vec2 sourceMax)
{
    auto const found = active.sprites.find(name);
    if (!textureId || found == active.sprites.end()) return false;
    Vec2 minimum{std::min(a.x, b.x), std::min(a.y, b.y)};
    Vec2 maximum{std::max(a.x, b.x), std::max(a.y, b.y)};
    if (minimum.x == maximum.x || minimum.y == maximum.y) return true;
    auto const& sprite = found->second;
    auto const& r = sprite.region;
    auto uv = [&](Vec2 fraction) {
        return Vec2{(r.x + 0.5f + fraction.x * (r.width - 1)) / active.width,
            (r.y + 0.5f + fraction.y * (r.height - 1)) / active.height};
    };
    list->AddImage(WorldDrawList::Texture::ObjectAtlas, minimum, maximum,
        uv(sourceMin), uv(sourceMax), sprite.tintable ? tint : White);
    return true;
}
