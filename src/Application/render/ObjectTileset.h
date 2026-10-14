#pragma once
#include "SectorTileset.h"
#include <optional>

namespace pf::application::render {
struct ObjectSprite { SectorTileRegion region; bool tintable; };
struct ObjectTileset
{
    int width{}, height{};
    std::map<std::string, ObjectSprite> sprites;
};
PF_APPLICATION_API void setObjectTileset(ObjectTileset tileset);
PF_APPLICATION_API void clearObjectTileset();
PF_APPLICATION_API void setAgentTileset(ObjectTileset tileset);
PF_APPLICATION_API void clearAgentTileset();
std::optional<Vec2> agentSpriteSize(char const* name);
bool drawAgentSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint = White);
bool hasObjectTileset();
std::optional<Vec2> objectSpriteSize(char const* name);
// Destination is the object's physical screen bounds, not its padded storage cell.
// Source fractions allow moving door leaves to be cropped without squashing.
bool drawObjectSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint = White, Vec2 sourceMin = {0, 0}, Vec2 sourceMax = {1, 1});

} // namespace pf::application::render
