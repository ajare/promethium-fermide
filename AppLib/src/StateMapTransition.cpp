#include <willpower/application/StateExceptions.h>
#include <willpower/application/ServiceLocator.h>
#include <willpower/application/ApplicationSettings.h>

#include "StateMapTransition.h"
#include "PlayObjectCreators.h"
#include "Map.h"

namespace applib {

using namespace std;
using namespace wp;

StateMapTransition::StateMapTransition(GeometryMeshRendererFactory* factory, bool useThreading)
    : ThreadableLoadState("MapTransition", Action::Unload, useThreading), mFactory(factory) {
}

ThreadableLoadState::LoadFunction StateMapTransition::getWorkFunction(wp::application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args) {
  WP_UNUSED(renderSystem);
  WP_UNUSED(renderResourceMgr);

  auto transitionData = static_cast<StateTransitionData*>(args);
  return bind(&StateMapTransition::processResources, this, resourceMgr, &transitionData->mapData);
}

void StateMapTransition::acquireMapResource(application::resourcesystem::ResourceManager* resourceMgr, application::resourcesystem::ResourcePtr resource, bool useThreading) {
  WP_UNUSED(useThreading);

  resourceMgr->acquireResource(resource);
}

vector<ThreadableLoadState::ThreadableWorkFunction> StateMapTransition::getPreWork(StateTransitionData* transitionData) {
  auto prevMapRenderer = transitionData->mapData.prevMap.mapRenderer ? transitionData->mapData.prevMap.mapRenderer.release() : nullptr;
  auto prevMapCollisionSim = transitionData->mapData.prevMap.mapCollisionSim ? transitionData->mapData.prevMap.mapCollisionSim.release() : nullptr;
  auto nextMap = transitionData->mapData.nextMap.map;

  auto destroyPrevMapRendererFn = [this, prevMapRenderer](bool useThreading) {
    addText("Destroying map renderer");
    destroyMapRenderer(prevMapRenderer, useThreading);
  };

  auto destroyPrevMapCollisionSimFn = [this, prevMapCollisionSim](bool useThreading) {
    addText("Destroying player collision sim");
    destroyMapCollisionSim(prevMapCollisionSim, useThreading);
  };

  // Acquire new map
  auto acquireNextMapFn = bind(&StateMapTransition::acquireMapResource,
                               this,
                               mwResourceMgr,
                               nextMap,
                               placeholders::_1);

  return {destroyPrevMapRendererFn, destroyPrevMapCollisionSimFn, acquireNextMapFn};
}

void StateMapTransition::processResources(application::resourcesystem::ResourceManager* resourceMgr, MapTransitionData* transitionData) {
  // Get transition data from previous state
  auto prevMap = transitionData->prevMap.map;
  auto nextMap = transitionData->nextMap.map;

  // Set up new transition data to pass to next state
  mTransitionData.mapData.nextMap.map = nextMap;

  // Release the previous map
  if (usingThreading()) {
    addPendingResourceName(prevMap->getName());
    addPendingResourceName(nextMap->getName());
  }

  mWillpowerResourcesToUnload = {prevMap};

  if (usingThreading()) {
    auto callback = bind(&StateMapTransition::unloadResourceCallback,
                         this,
                         placeholders::_1,
                         placeholders::_2,
                         placeholders::_3);

    for (auto resource : mWillpowerResourcesToUnload) {
      resourceMgr->releaseResource(resource, callback);
    }

    mWillpowerResourcesToUnload.clear();
  }

  // Load next map
  mWillpowerResourcesToLoad = {nextMap};

  if (usingThreading()) {
    auto loadCallbackFn = bind(&StateMapTransition::loadResourceCallback,
                               this,
                               placeholders::_1,
                               placeholders::_2,
                               placeholders::_3);

    for (auto resource : mWillpowerResourcesToLoad) {
      resourceMgr->createResource(resource, loadCallbackFn);
      resourceMgr->loadResource(resource, loadCallbackFn);
    }

    mWillpowerResourcesToLoad.clear();
  }

  // Create map renderer for next map
  auto createNextMapRendererFn = [this, nextMap](bool useThreading) {
    this->addText("Creating map renderer");
    auto renderer = createMapRenderer(nextMap, this->mwRenderSystem, this->mwRenderResourceMgr, mFactory, useThreading);
    this->mTransitionData.mapData.nextMap.mapRenderer = renderer ? unique_ptr<viz::GeometryMeshRenderer>(renderer) : nullptr;
  };

  // Create map collision sim for next map
  auto createNextMapCollisionSimFn = [this, nextMap](bool useThreading) {
    this->addText("Creating player collision sim");
    auto collisionSim = createMapCollisionSim(nextMap, this->mwRenderSystem, this->mwRenderResourceMgr, useThreading);
    this->mTransitionData.mapData.nextMap.mapCollisionSim = collisionSim ? unique_ptr<collide::Simulation>(collisionSim) : nullptr;
  };

  processPostWork({createNextMapRendererFn, createNextMapCollisionSimFn});
}

}  // namespace applib