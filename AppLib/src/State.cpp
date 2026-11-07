#include <mpp/helper/OrthoCamera.h>

#include <willpower/application/StateExceptions.h>
#include <willpower/application/ServiceLocator.h>
#include <willpower/application/ApplicationSettings.h>

#include "State.h"

namespace applib {

using namespace std;
using namespace wp;

State::State(string const& name)
    : application::State(name), mwLogger(nullptr), mwResourceMgr(nullptr), mwAudioSystem(nullptr), mwRenderSystem(nullptr), mwRenderResourceMgr(nullptr) {
  mTransitionData.prevStateName = name;
}

wp::Vector2 State::getWindowSize() const {
  return {
      (float)mwRenderSystem->getWindowWidth(),
      (float)mwRenderSystem->getWindowHeight()};
}

void State::setLogger(wp::Logger* logger) {
  mwLogger = logger;
}

void State::enterImpl(wp::application::resourcesystem::ResourceManager* resourceMgr, wp::application::AudioSystem* audioSystem, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args) {
  mwLogger->info("Entering state: " + getName());

  mwResourceMgr = resourceMgr;
  mwAudioSystem = audioSystem;
  mwRenderSystem = renderSystem;
  mwRenderResourceMgr = renderResourceMgr;

  // Create MPP objects
  mRenderPipeline = renderSystem->getOrCreateRenderPipeline(getName());

  mScene = renderSystem->createScene("Default");
  mScene->load();

  mCamera = make_shared<mpp::helper::OrthoCamera>(
      glm::vec2(0.0f, 0.0f),
      renderSystem->getWindowWidth(),
      renderSystem->getWindowHeight());

  // Implementation-specific setup
  setup(resourceMgr, renderSystem, renderResourceMgr, args);
}

void State::exitImpl() {
  // Implementation-specific teardown
  teardown();

  mwLogger->info("Exiting state: " + getName());

  // Clean up
  mCamera.reset();

  mScene->unload();

  // RenderSystem keeps its own reference to the pipeline under this state's
  // name (see enterImpl), so resetting mRenderPipeline alone would not
  // destroy it - RenderSystem's copy, and anything its render-graph
  // callbacks captured (e.g. the last camera rendered with it), would
  // survive until RenderSystem itself is torn down. No other state shares
  // this name, so it is safe to evict it here.
  mwRenderSystem->removeRenderPipeline(getName());
  mRenderPipeline.reset();
}

void State::loadAllReferencedResources() {
  auto mppResources = mwRenderResourceMgr->getAllReferencedResources();
  for (auto res : mppResources) {
    if (!res->isLoaded()) {
      res->load();
    }
  }
}

vector<string> State::getDebuggingText() const {
  vector<string> lines;
  return lines;
}

}  // namespace applib