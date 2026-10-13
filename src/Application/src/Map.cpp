#include <utility>

#include <core/World.h>

#include "Map.h"

namespace pf::application {

Map::Map(std::string const& name, std::string const& namesp,
         std::string const& source, std::map<std::string, std::string> const& tags,
         wp::application::resourcesystem::ResourceLocation* location)
    : applib::Map(name, namesp, source, tags, location, 512.0f) {}

Map::~Map() = default;

core::World* Map::getWorld() noexcept {
  return mWorld.get();
}

core::World const* Map::getWorld() const noexcept {
  return mWorld.get();
}

void Map::setWorld(std::unique_ptr<core::World> world) noexcept {
  mWorld = std::move(world);
}

void Map::clearWorld() noexcept {
  mWorld.reset();
}

MapResourceFactory::MapResourceFactory()
    : wp::application::resourcesystem::ResourceFactory("Map") {}

wp::application::resourcesystem::Resource* MapResourceFactory::createResource(
    std::string const& name, std::string const& namesp,
    std::string const& source, std::map<std::string, std::string> const& tags,
    wp::application::resourcesystem::ResourceLocation* location) {
  return new Map(name, namesp, source, tags, location);
}

}  // namespace pf::application
