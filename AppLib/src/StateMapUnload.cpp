#include <willpower/application/StateExceptions.h>
#include <willpower/application/ServiceLocator.h>
#include <willpower/application/ApplicationSettings.h>

#include "StateMapUnload.h"
#include "PlayObjectCreators.h"

namespace applib {

using namespace std;
using namespace wp;

StateMapUnload::StateMapUnload(bool useThreading)
    : ThreadableLoadState("MapUnload", Action::Unload, useThreading) {
}

ThreadableLoadState::LoadFunction StateMapUnload::getWorkFunction(wp::application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args) {
  WP_UNUSED(renderSystem);
  WP_UNUSED(renderResourceMgr);

  auto transitionData = static_cast<StateTransitionData*>(args);
  return bind(&StateMapUnload::unloadResources, this, resourceMgr, &transitionData->mapData);
}

vector<ThreadableLoadState::ThreadableWorkFunction> StateMapUnload::getPreWork(StateTransitionData* transitionData) {
  auto mapRenderer = transitionData->mapData.prevMap.mapRenderer ? transitionData->mapData.prevMap.mapRenderer.release() : nullptr;
  auto mapCollisionSim = transitionData->mapData.prevMap.mapCollisionSim ? transitionData->mapData.prevMap.mapCollisionSim.release() : nullptr;

  auto destroyPrevMapRendererFn = [this, mapRenderer](bool useThreading) {
    addText("Destroying map renderer");
    destroyMapRenderer(mapRenderer, useThreading);
  };

  auto destroyPrevMapCollisionSimFn = [this, mapCollisionSim](bool useThreading) {
    addText("Destroying player collision sim");
    destroyMapCollisionSim(mapCollisionSim, useThreading);
  };

  return {destroyPrevMapRendererFn, destroyPrevMapCollisionSimFn};
}

void StateMapUnload::unloadResources(application::resourcesystem::ResourceManager* resourceMgr, MapTransitionData* transitionData) {
  // Unload given map & its resources
  string mapName = transitionData->prevMap.map->getName();
  string mapNamespace = transitionData->prevMap.map->getNamespace();

  // Willpower resources
  mWillpowerResourcesToLoad.clear();
  mWillpowerResourcesToUnload = resourceMgr->getNamespaceResources(mapNamespace);

  if (usingThreading()) {
    int resourceCount = (int)mWillpowerResourcesToUnload.size();

    if (resourceCount > 0) {
      for (auto res : mWillpowerResourcesToUnload) {
        addPendingResourceName(res->getName());
      }

      auto callback = bind(&StateMapUnload::unloadResourceCallback,
                           this,
                           placeholders::_1,
                           placeholders::_2,
                           placeholders::_3);

      for (auto resource : mWillpowerResourcesToUnload) {
        resourceMgr->releaseResource(resource, callback);
      }

      mWillpowerResourcesToUnload.clear();
    }
  }
}

}  // namespace applib