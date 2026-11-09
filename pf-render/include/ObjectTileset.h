#pragma once
#include "SectorTileset.h"
#include <optional>
struct ObjectSprite { SectorTileRegion region; bool tintable; };
struct ObjectTileset
{
    std::filesystem::path image;
    int width{}, height{};
    std::map<std::string, ObjectSprite> sprites;
    static ObjectTileset load(std::filesystem::path const& path);
};
void setObjectTileset(ObjectTileset tileset, ImTextureID texture);
void clearObjectTileset();
// Agent artwork is independently selected, even when it shares the image.
void setAgentTileset(ObjectTileset tileset, ImTextureID texture);
void clearAgentTileset();
// The sprite helpers cross to the command stream on the shared drawing
// types (#997): pf::render::Vec2 for the destination rectangle and a
// packed colour for the tint.
std::optional<Vec2> agentSpriteSize(char const* name);
bool drawAgentSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint = White);
void setFurnitureTileset(ObjectTileset tileset, ImTextureID texture);
void clearFurnitureTileset();
std::optional<Vec2> furnitureSpriteSize(char const* name);
bool drawFurnitureSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b);
bool hasObjectTileset();
std::optional<Vec2> objectSpriteSize(char const* name);
// Destination is the object's physical screen bounds, not its padded storage cell.
// Source fractions allow moving door leaves to be cropped without squashing.
bool drawObjectSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint = White, Vec2 sourceMin = {0, 0}, Vec2 sourceMax = {1, 1});

// No ImDrawList-taking form: see SectorTileset.h. The headless harness
// records a command vector and replays it (#1000).
