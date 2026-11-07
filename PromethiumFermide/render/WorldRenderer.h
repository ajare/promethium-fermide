#pragma once

#include <memory>
#include <mpp/Scene.h>
#include "Application.h"
#include "WorldDrawList.h"

namespace core { class World; }
namespace mpp { class ResourceManager; }
namespace wp::application::resourcesystem { class ResourceManager; }

namespace pf::application::render {
// Application-only viewport: follows the first Player-controlled Agent and its
// Layer, or fits Layer 0 when none exists. No simulation updates, editor globals,
// debug overlays or ImGui context.
//
// Takes the World by const reference (#970). The game's viewport is handed the
// read view its WorldRuntime publishes, so the renderer cannot mutate what it
// draws - and cannot keep that World alive past the runtime that owns it either.
PF_APPLICATION_API WorldDrawList drawWorld(core::World const& world,
                                          float width, float height);
class WorldRenderer {
  class Impl;
  std::unique_ptr<Impl> mImpl;
public:
  WorldRenderer(wp::application::resourcesystem::ResourceManager& resources,
                mpp::RenderSystem& renderSystem, mpp::ResourceManager& renderResources,
                mpp::ScenePtr scene, core::World const& world);
  ~WorldRenderer();
  void render();
};
} // namespace pf::application::render
