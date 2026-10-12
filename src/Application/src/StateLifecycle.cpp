#include <willpower/application/StateExceptions.h>

#include "StateLifecycle.h"

namespace pf::application {

StateLifecycle::StateLifecycle(std::string const& name) : applib::State(name) {}

void StateLifecycle::setup(wp::application::resourcesystem::ResourceManager*,
                           mpp::RenderSystem*, mpp::ResourceManager*, void*) {
  mScene->setClearColour(mpp::Colour::Black);
}

void StateLifecycle::teardown() {}

void StateLifecycle::updateImpl(float) {
  throw wp::application::ReturnFromStateException(&mTransitionData);
}

void StateLifecycle::renderImpl(mpp::RenderSystem* renderSystem,
                                mpp::ResourceManager*) {
  renderSystem->renderScene(mScene, mCamera, glm::vec2(0.0f), getName());
}

}  // namespace pf::application
