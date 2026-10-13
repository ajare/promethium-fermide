#pragma once

#include <memory>
#include <mpp/Scene.h>
#include "Application.h"
#include "WorldDrawList.h"

namespace core { class World; }
namespace mpp { class ResourceManager; }
namespace wp::application::resourcesystem { class ResourceManager; }

namespace pf::application::render {
// Application-only viewport: Layer 0, centred and fitted to the window. No
// simulation updates, editor globals, debug overlays or ImGui context.
PF_APPLICATION_API WorldDrawList drawWorld(std::shared_ptr<const core::World> const& world,
                                          float width, float height);
class WorldRenderer {
  class Impl;
  std::unique_ptr<Impl> mImpl;
public:
  WorldRenderer(wp::application::resourcesystem::ResourceManager& resources,
                mpp::RenderSystem& renderSystem, mpp::ResourceManager& renderResources,
                mpp::ScenePtr scene, std::shared_ptr<const core::World> world);
  ~WorldRenderer();
  void render();
};
} // namespace pf::application::render
