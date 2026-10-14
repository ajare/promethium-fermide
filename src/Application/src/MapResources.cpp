#include <stdexcept>
#include <core/World.h>

#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>

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

void MapResources::load(std::function<void()> postFunction) {
  if (mMap) return;
  auto map = std::dynamic_pointer_cast<Map>(mManager.getResource("World", "World"));
  if (!map) throw std::runtime_error("World/World must be an Application Map");
  map->getDependentResource("World"); // Validate cleanup's dependency before acquisition.
  mManager.acquireResource(map);
  mMap = std::move(map);
  try {
    mManager.createResource(mMap);
    mManager.loadResource(mMap);
    if (!mMap->getWorld() || !mManager.isResourceLoaded(mMap))
      throw std::runtime_error("World/World did not finish loading");
    if (postFunction) postFunction();
  } catch (...) {
    unload();
    throw;
  }
}

void MapResources::loadAtlases() {
  if (!mMap || !mManager.isResourceLoaded(mMap))
    throw std::runtime_error("Atlas post work requires a loaded Map");
  std::array names{mMap->getWorld()->sectorImageSetResourceName(), std::string{"ObjectAtlas"}};
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (mAtlases[index]) continue;
    std::shared_ptr<wp::application::resourcesystem::ImageSetResource> atlas;
    try {
      atlas = std::dynamic_pointer_cast<wp::application::resourcesystem::ImageSetResource>(
          mManager.getResource(names[index]));
    } catch (std::exception const&) { if (index != 0) throw; }
    if (!atlas) {
      if (index == 0) continue; // Explicit invalid artwork is recoverable.
      throw std::runtime_error("Application atlas must be an ImageSet");
    }
    atlas->getImage(); // Validate cleanup's dependency before acquisition.
    mManager.acquireResource(atlas);
    mAtlases[index] = atlas;
    try {
      mManager.createResource(atlas);
      mManager.loadResource(atlas);
      if (!mManager.isResourceLoaded(atlas) || !mManager.isResourceLoaded(atlas->getImage()))
        throw std::runtime_error("Application atlas did not finish loading");
    } catch (std::exception const&) {
      if (index != 0) throw;
      mManager.releaseResource(atlas);
      mAtlases[index].reset();
    }
  }
}

void MapResources::unload() {
  for (auto it = mAtlases.rbegin(); it != mAtlases.rend(); ++it) {
    if (!*it) continue;
    auto atlas = std::static_pointer_cast<wp::application::resourcesystem::ImageSetResource>(
        std::move(*it));
    auto image = atlas->getImage();
    mManager.releaseResource(atlas);
    if (!mManager.isResourceLoaded(atlas)) mManager.destroyResources({atlas});
    if (!mManager.isResourceLoaded(image)) mManager.destroyResources({image});
  }
  if (!mMap) return;
  auto map = std::move(mMap); // Release our acquisition exactly once.
  auto source = map->getDependentResource("World");
  mManager.releaseResource(map);
  // Respect any other consumer's acquisition. With no other owner, destroy
  // created data as well as unloading: the next run must deserialize afresh.
  if (!mManager.isResourceLoaded(map)) {
    map->clearWorldResource();
    mManager.destroyResources({map});
  }
  if (!mManager.isResourceLoaded(source)) mManager.destroyResources({source});
}

}  // namespace pf::application
