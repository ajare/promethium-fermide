#pragma once

#include <array>
#include <functional>
#include <memory>
#include <willpower/application/resourcesystem/Resource.h>

#include "Application.h"

namespace pf::application {
class Map;

// Controller owns this guard; ResourceManager outlives Controller's states.
// Children borrow it through StateTransitionData::userData.
class PF_APPLICATION_API MapResources final {
  wp::application::resourcesystem::ResourceManager& mManager;
  wp::Logger* mLogger;
  std::shared_ptr<Map> mMap;
  std::array<wp::application::resourcesystem::ResourcePtr, 4> mAtlases;

public:
  explicit MapResources(wp::application::resourcesystem::ResourceManager& manager,
                        wp::Logger* logger = nullptr);
  ~MapResources();
  MapResources(MapResources const&) = delete;
  MapResources& operator=(MapResources const&) = delete;

  Map* getMap() const { return mMap.get(); }

  void scan();
  // Run graphics-dependent post work after the World has finished loading.
  // Any post-work failure rolls back the entire MapLoad acquisition.
  void load(std::function<void()> postFunction = {});
  void loadAtlases();
  void unload();
};

}  // namespace pf::application
