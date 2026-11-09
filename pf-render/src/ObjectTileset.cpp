#include "ObjectTileset.h"
#include "AuthoredTileGrid.h"
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
    // The grid resolution is shared with the sector atlas, which authors the
    // same cell-and-content shape (#1017).
    auto const grid = loadAuthoredTileGrid(root, version >= 2, "object sprite");
    result.width = grid.width;
    result.height = grid.height;
    result.sprites = grid.tiles;
    // A version 2 set authors nothing for the Force Bridge, and is refused
    // if it tries. A bridge's span is continuous and arbitrary, so a fixed
    // tile would be stretched wrong at most lengths it is drawn at; the
    // renderer's primitive line stays. Refusing it here is what keeps that
    // decision from being quietly reversed by a tile added to a sheet
    // nobody thought about (#1019).
    if (version >= 2 && result.sprites.count("force-bridge"))
        throw std::runtime_error("The Force Bridge is rendered as a primitive "
            "line and is deliberately given no tile: its span is arbitrary, so "
            "a fixed tile would be wrong at most lengths");
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
    auto const& sprite = agents.sprites.at(name);
    auto const& r = sprite.region;
    Vec2 uv0{(r.x + .5f) / agents.width, (r.y + .5f) / agents.height};
    Vec2 uv1{(r.x + r.width - .5f) / agents.width, (r.y + r.height - .5f) / agents.height};
    // Whether the resolved Agent colour reaches the sheet at all is the tile's
    // authored fact, exactly as it is for an Object sprite. The Agent's colour,
    // its Colour tags and the selection gold are still resolved by the caller;
    // `tintable` says whether this artwork takes a tint (#1018).
    list->AddImage(WorldDrawList::Texture::AgentAtlas, a, b, uv0, uv1,
        sprite.tintable ? tint : White);
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
