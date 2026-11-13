#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <willpower/application/resourcesystem/Resource.h>

#include "Application.h"

namespace pf::application {
class Map;

// Controller owns this guard; ResourceManager outlives Controller's states.
// Children borrow it through StateTransitionData::userData.
class PF_APPLICATION_API MapResources final {
  // One atlas this guard holds: the name it was asked for, and the resource
  // taken up under it. The name travels with the resource because the load
  // step deduplicates by name - two Layers bound to one sheet is one
  // acquisition - and a held list that cannot say which sheet each entry
  // is cannot be checked against the World that named it.
  struct HeldAtlas {
    std::string name;
    wp::application::resourcesystem::ResourcePtr atlas;
  };

  wp::application::resourcesystem::ResourceManager& mManager;
  wp::Logger* mLogger;
  std::shared_ptr<Map> mMap;
  // The atlases taken up so far, in acquisition order. A World names more
  // than one Sector sheet - its own plus one binding per Layer - so this is
  // a list and not a fixed row of slots, and it grows to whatever the World
  // asks for. The Sector family comes first, so the reverse walk in
  // unload() puts the World's own sheet down last: it is the sheet every
  // unbound Layer falls back to, and the last thing still standing when the
  // World comes down.
  std::vector<HeldAtlas> mAtlases;

  bool holdsAtlas(std::string const& name) const;
  void reportUnusable(std::string const& name, std::string const& reason) const;

public:
  explicit MapResources(wp::application::resourcesystem::ResourceManager& manager,
                        wp::Logger* logger = nullptr);
  ~MapResources();
  MapResources(MapResources const&) = delete;
  MapResources& operator=(MapResources const&) = delete;

  Map* getMap() const { return mMap.get(); }

  // The atlas names held right now, in the order they were taken up. A
  // sheet two Layers both name appears once: the guard acquires it once and
  // releases it once.
  std::vector<std::string> getAtlasNames() const;

  void scan();
  // Run graphics-dependent post work after the World has finished loading.
  // Any post-work failure rolls back the entire MapLoad acquisition.
  void load(std::function<void()> postFunction = {});
  void loadAtlases();
  void unload();
};

}  // namespace pf::application
