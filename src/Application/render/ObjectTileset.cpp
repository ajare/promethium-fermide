#include "ObjectTileset.h"
#include "core/Defines.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pf::application::render {
namespace {
ObjectTileset active;
ObjectTileset agents;
}
void setObjectTileset(ObjectTileset tileset)
{
    active = std::move(tileset);
}
void clearObjectTileset() { active = {}; }
void setAgentTileset(ObjectTileset tileset) { agents = std::move(tileset); }
void clearAgentTileset() { agents = {}; }
std::optional<Vec2> agentSpriteSize(char const* name)
{
    auto const found = agents.sprites.find(name);
    if (agents.width <= 0 || agents.height <= 0 || found == agents.sprites.end()) return std::nullopt;
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
bool hasObjectTileset() { return active.width > 0 && active.height > 0; }
std::optional<Vec2> objectSpriteSize(char const* name)
{
    auto const sprite = active.sprites.find(name);
    if (!hasObjectTileset() || sprite == active.sprites.end()) return std::nullopt;
    return Vec2{static_cast<float>(sprite->second.region.width),
        static_cast<float>(sprite->second.region.height)};
}
bool drawObjectSprite(char const* name, WorldDrawList* list, Vec2 a, Vec2 b,
    PackedColourValue tint, Vec2 sourceMin, Vec2 sourceMax)
{
    auto const found = active.sprites.find(name);
    if (!hasObjectTileset() || found == active.sprites.end()) return false;
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

} // namespace pf::application::render
