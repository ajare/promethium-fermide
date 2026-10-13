#include <willpower/common/ExtentsCalculator.h>

#include "PlayObjectCreators.h"
#include "Map.h"

namespace applib {
using namespace std;
using namespace wp;

viz::GeometryMeshRenderer* createMapRenderer(application::resourcesystem::ResourcePtr resource, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, GeometryMeshRendererFactory* factory, bool useThreading) {
  WP_UNUSED(useThreading);

  auto map = static_cast<Map*>(resource.get());
  auto mapMesh = map->getMesh();

  if (mapMesh) {
    auto gridOptions = map->getGridOptions();

    // auto mapRenderer = new viz::GeometryMeshRenderer(resource->getName() + "_Renderer", mapMesh, gridOptions, 32, renderResourceMgr);
    auto mapRenderer = factory->create(resource->getName() + "_Renderer", mapMesh, gridOptions, 32, renderResourceMgr);
    mapRenderer->build(renderSystem, renderResourceMgr);

    return mapRenderer;
  } else {
    return nullptr;
  }
}

void destroyMapRenderer(viz::GeometryMeshRenderer* mapRenderer, bool useThreading) {
  WP_UNUSED(useThreading);

  delete mapRenderer;
}

void buildCollisionSimulation(collide::Simulation* sim, shared_ptr<wp::geometry::Mesh> mesh) {
  sim->addMesh(mesh.get());
}

collide::Simulation* createMapCollisionSim(wp::application::resourcesystem::ResourcePtr resource, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, bool useThreading) {
  WP_UNUSED(useThreading);

  auto mapMesh = static_cast<Map*>(resource.get())->getMesh();

  if (mapMesh) {
    Vector2 minExtent, maxExtent;
    mapMesh->getExtents(minExtent, maxExtent);

    ExtentsCalculator extents(minExtent, maxExtent, 1.0f);

    auto mapCollisionSim = new collide::Simulation(extents, 8, 8);
    buildCollisionSimulation(mapCollisionSim, mapMesh);

    return mapCollisionSim;
  } else {
    return nullptr;
  }
}

void destroyMapCollisionSim(collide::Simulation* sim, bool useThreading) {
  WP_UNUSED(useThreading);

  delete sim;
}

}  // namespace applib