#include <stdexcept>
#include <willpower/application/StateExceptions.h>

#include "StateLifecycle.h"
#include "MapResources.h"

namespace pf::application {

StateLifecycle::StateLifecycle(std::string const& name) : applib::State(name) {}

void StateLifecycle::setup(wp::application::resourcesystem::ResourceManager*,
                           mpp::RenderSystem*, mpp::ResourceManager*, void* args) {
  resumeImpl(args);
  mScene->setClearColour(mpp::Colour::Black);
}

void StateLifecycle::teardown() {}

void StateLifecycle::resumeImpl(void* args) {
  if (!args) throw std::runtime_error("Missing application transition data");
  mTransitionData.userData = static_cast<applib::StateTransitionData*>(args)->userData;
}

void StateLifecycle::updateImpl(float) {
  auto* resources = static_cast<MapResources*>(mTransitionData.userData);
  if (!resources) throw std::runtime_error("Missing application resource lifecycle");
  if (getName() == "Load") resources->scan();
  else if (getName() == "MapLoad") {
    resources->load([resources] { resources->loadAtlases(); });
  }
  else if (getName() == "MapUnload" || getName() == "Unload") resources->unload();
  throw wp::application::ReturnFromStateException(&mTransitionData);
}

void StateLifecycle::renderImpl(mpp::RenderSystem* renderSystem,
                                mpp::ResourceManager*) {
  renderSystem->renderScene(mScene, mCamera, glm::vec2(0.0f), getName());
}

}  // namespace pf::application
