#include <utility>
#include <stdexcept>

#include <core/World.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

#include "ApplicationResources.h"
#include "Map.h"
#include "MapWorldDefinitionFactory.h"

namespace pf::application {

void registerMapResourceTypes(wp::application::resourcesystem::ResourceManager& manager) {
  manager.addResourceFactory(new MapResourceFactory());
  manager.addResourceDefinitionFactory(new MapWorldDefinitionFactory());
}

Map::Map(std::string const& name, std::string const& namesp,
         std::string const& source, std::map<std::string, std::string> const& tags,
         wp::application::resourcesystem::ResourceLocation* location)
    : applib::Map(name, namesp, source, tags, location, 512.0f) {}

Map::~Map() = default;

core::World* Map::getWorld() noexcept {
  return mWorldResource ? mWorldResource->world().get() : nullptr;
}

core::World const* Map::getWorld() const noexcept {
  return mWorldResource ? mWorldResource->world().get() : nullptr;
}

std::shared_ptr<WorldResource> const& Map::getWorldResource() const noexcept {
  return mWorldResource;
}

void Map::setWorldResource(std::shared_ptr<WorldResource> resource) {
  if (resource && !resource->world())
    throw std::invalid_argument("Map requires a created World resource");
  mWorldResource = std::move(resource);
}

void Map::clearWorldResource() noexcept {
  mWorldResource.reset();
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
