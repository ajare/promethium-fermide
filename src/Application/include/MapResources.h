#pragma once

#include <memory>

#include "Application.h"

namespace pf::application {
class Map;

// Controller owns this guard; ResourceManager outlives Controller's states.
// Children borrow it through StateTransitionData::userData.
class PF_APPLICATION_API MapResources final {
  wp::application::resourcesystem::ResourceManager& mManager;
  wp::Logger* mLogger;
  std::shared_ptr<Map> mMap;

public:
  explicit MapResources(wp::application::resourcesystem::ResourceManager& manager,
                        wp::Logger* logger = nullptr);
  ~MapResources();
  MapResources(MapResources const&) = delete;
  MapResources& operator=(MapResources const&) = delete;

  Map* getMap() const { return mMap.get(); }

  void scan();
  void load();
  void unload();
};

}  // namespace pf::application
