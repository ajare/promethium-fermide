#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <game/WorldRuntime.h>

#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>

#include "Map.h"
#include "MapResources.h"

namespace pf::application {

namespace {

// One sheet the atlas load step was asked to take up.
struct AtlasRequest {
  std::string name;
  // The Objects atlas is the one sheet the game cannot start without: the
  // object renderer draws a World's Doors, Windows, Lifts and Shuttles at
  // the extents that set declares, so a World with no usable Objects atlas
  // is a World that cannot be drawn at all. Every other sheet - the
  // World's own Sector sheet and each Layer's override - is recoverable:
  // the canvas paints magenta where the art should be and the game plays
  // on.
  bool mandatory{false};
};

}  // namespace

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
    if (!mMap->getRuntime() || !mManager.isResourceLoaded(mMap))
      throw std::runtime_error("World/World did not finish loading");
    if (postFunction) postFunction();
  } catch (...) {
    unload();
    throw;
  }
}

bool MapResources::holdsAtlas(std::string const& name) const {
  for (auto const& held : mAtlases)
    if (held.name == name) return true;
  return false;
}

void MapResources::reportUnusable(std::string const& name,
                                 std::string const& reason) const {
  // A sheet the game could not take up paints magenta where its art should
  // be: visible, but silent about why. The four World-wide slots get a
  // report from the render pass every frame; a Layer's binding has no such
  // pass behind it, so this is the only place a refused Layer sheet says
  // what went wrong. A recovery nobody is told about is indistinguishable
  // from a sheet that was never asked for.
  if (mLogger) mLogger->error("Atlas '" + name + "' " + reason);
}

std::vector<std::string> MapResources::getAtlasNames() const {
  std::vector<std::string> names;
  names.reserve(mAtlases.size());
  for (auto const& held : mAtlases) names.push_back(held.name);
  return names;
}

void MapResources::loadAtlases() {
  if (!mMap || !mManager.isResourceLoaded(mMap))
    throw std::runtime_error("Atlas post work requires a loaded Map");
  auto const& world = mMap->getRuntime()->world();

  // Every sheet this World draws from, in the order they are taken up.
  //
  // The Sector family leads with the World's own sheet and follows with
  // each Layer's override, so the reverse walk in unload() puts the
  // World's sheet down last. A Layer with no binding of its own inherits
  // the World's sheet, so it asks for nothing; a binding is a name, and an
  // empty name is a binding to nothing rather than an inheritance.
  //
  // A name is asked for once however many places name it. Two Layers bound
  // to one sheet is one acquisition, not two, and a name that is also a
  // World-wide slot is held under the stricter of the two rules: the
  // Objects atlas does not stop being mandatory because a Layer happened
  // to name it first.
  std::vector<AtlasRequest> requests;
  auto const ask = [&requests](std::string const& name, bool mandatory) {
    for (auto& existing : requests) {
      if (existing.name != name) continue;
      existing.mandatory = existing.mandatory || mandatory;
      return;
    }
    requests.push_back({name, mandatory});
  };

  ask(world.sectorImageSetResourceName(), false);
  for (std::uint32_t layer = 0; layer < world.getLayerCount(); ++layer) {
    auto const& binding = world.layerSectorImageSet(layer);
    if (binding) ask(*binding, false);
  }
  ask(world.objectImageSetResourceName(), true);
  ask(world.agentImageSetResourceName(), false);
  ask(world.furnitureImageSetResourceName(), false);

  for (auto const& request : requests) {
    if (holdsAtlas(request.name)) continue;
    std::shared_ptr<wp::application::resourcesystem::ImageSetResource> atlas;
    try {
      atlas = std::dynamic_pointer_cast<wp::application::resourcesystem::ImageSetResource>(
          mManager.getResource(request.name));
    } catch (std::exception const& error) {
      if (request.mandatory) throw;
      reportUnusable(request.name, "could not be resolved: " + std::string(error.what()));
      continue;
    }
    if (!atlas) {
      // Explicit invalid artwork is recoverable.
      if (request.mandatory) throw std::runtime_error("Application atlas must be an ImageSet");
      reportUnusable(request.name, "is not an Image set resource");
      continue;
    }
    atlas->getImage(); // Validate cleanup's dependency before acquisition.
    mManager.acquireResource(atlas);
    mAtlases.push_back({request.name, atlas});
    try {
      mManager.createResource(atlas);
      mManager.loadResource(atlas);
      if (!mManager.isResourceLoaded(atlas) || !mManager.isResourceLoaded(atlas->getImage()))
        throw std::runtime_error("Application atlas did not finish loading");
    } catch (std::exception const& error) {
      if (request.mandatory) throw;
      // Reported before the release, so a release that itself throws still
      // leaves the reason on the record. The entry stays standing until the
      // release is through, so a throwing release leaves the acquisition
      // where unload() can find it rather than orphaning it.
      reportUnusable(request.name, "did not finish loading: " + std::string(error.what()));
      mManager.releaseResource(atlas);
      mAtlases.pop_back();
    }
  }
}

void MapResources::unload() {
  for (auto it = mAtlases.rbegin(); it != mAtlases.rend(); ++it) {
    if (!it->atlas) continue;
    // Moved out before anything can throw, so a release that fails cannot
    // leave the same acquisition standing to be released a second time.
    auto atlas = std::static_pointer_cast<wp::application::resourcesystem::ImageSetResource>(
        std::move(it->atlas));
    auto image = atlas->getImage();
    mManager.releaseResource(atlas);
    // Only destroyed when no other holder is left standing: a sheet the
    // editor or another consumer still holds outlives this guard.
    if (!mManager.isResourceLoaded(atlas)) mManager.destroyResources({atlas});
    if (!mManager.isResourceLoaded(image)) mManager.destroyResources({image});
  }
  mAtlases.clear();
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
