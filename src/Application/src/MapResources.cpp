#include <stdexcept>

#include <willpower/application/resourcesystem/ResourceManager.h>

#include "Map.h"
#include "MapResources.h"

namespace pf::application {

MapResources::MapResources(
    wp::application::resourcesystem::ResourceManager& manager, wp::Logger* logger)
    : mManager(manager), mLogger(logger) {}

MapResources::~MapResources() {
  try {
    unload();
  } catch (std::exception const& error) {
    // ResourceManager remains the final cleanup owner if state teardown fails.
    try { if (mLogger) mLogger->error(error.what()); } catch (...) {}
  } catch (...) {}
}

void MapResources::scan() {
  mManager.scanLocations();
}

void MapResources::load() {
  if (mMap) return;
  auto map = std::dynamic_pointer_cast<Map>(mManager.getResource("World", "World"));
  if (!map) throw std::runtime_error("World/World must be an Application Map");
  map->getDependentResource("Yaml"); // Validate cleanup's dependency before acquisition.
  mManager.acquireResource(map);
  mMap = std::move(map);
  try {
    mManager.createResource(mMap);
    mManager.loadResource(mMap);
    if (!mMap->getWorld() || !mManager.isResourceLoaded(mMap))
      throw std::runtime_error("World/World did not finish loading");
  } catch (...) {
    unload();
    throw;
  }
}

void MapResources::unload() {
  if (!mMap) return;
  auto map = std::move(mMap); // Release our acquisition exactly once.
  auto source = map->getDependentResource("Yaml");
  mManager.releaseResource(map);
  // Respect any other consumer's acquisition. With no other owner, destroy
  // created data as well as unloading: the next run must deserialize afresh.
  if (!mManager.isResourceLoaded(map)) {
    map->clearWorld();
    mManager.destroyResources({map});
  }
  if (!mManager.isResourceLoaded(source)) mManager.destroyResources({source});
}

}  // namespace pf::application
