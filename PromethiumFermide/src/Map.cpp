#include <utility>
#include <stdexcept>

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

std::shared_ptr<game::WorldRuntime> Map::getRuntime() const noexcept {
  // Read through to the resource rather than caching the runtime: the
  // resource's destroy() is what actually frees the World, and a held
  // runtime would keep it alive past that (#970).
  return mWorldResource ? mWorldResource->runtime() : nullptr;
}

void Map::setWorldResource(std::shared_ptr<WorldResource> resource) {
  if (resource && !resource->runtime())
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
