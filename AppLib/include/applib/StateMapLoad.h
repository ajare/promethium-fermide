#pragma once

#include <vector>
#include <memory>

#include <willpower/application/StateFactory.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/application/resourcesystem/ResourceCallback.h>

#include <willpower/collide/Simulation.h>

#include "Platform.h"
#include "ThreadableLoadState.h"
#include "GeometryMeshRendererFactory.h"

namespace applib {

class APPLIB_API StateMapLoad : public ThreadableLoadState {
  GeometryMeshRendererFactory* mFactory;

private:
  LoadFunction getWorkFunction(wp::application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args = nullptr) override;

  virtual void loadResources(wp::application::resourcesystem::ResourceManager* resourceMgr, MapTransitionData* transitionData);

public:
  StateMapLoad(GeometryMeshRendererFactory* factory, bool useThreading);
};

class StateMapLoadFactory : public wp::application::StateFactory {
protected:
  wp::Logger* mwLogger;

  wp::application::resourcesystem::ResourceManager* mwResourceMgr;

  GeometryMeshRendererFactory* mFactory;

  bool mUseThreading;

public:
  StateMapLoadFactory(wp::Logger* logger, wp::application::resourcesystem::ResourceManager* resourceMgr, GeometryMeshRendererFactory* factory, bool useThreading)
      : wp::application::StateFactory("MapLoad"), mwLogger(logger), mwResourceMgr(resourceMgr), mFactory(factory), mUseThreading(useThreading) {
  }

  wp::application::State* createState() {
    auto state = new StateMapLoad(mFactory, mUseThreading);
    state->setLogger(mwLogger);
    return state;
  }
};

}  // namespace applib