#include <stdexcept>
#include <willpower/application/StateExceptions.h>

#include "StatePlay.h"

namespace pf::application {

StatePlay::StatePlay() : applib::State("Play") {}

void StatePlay::setup(wp::application::resourcesystem::ResourceManager*,
                      mpp::RenderSystem*, mpp::ResourceManager*, void* args) {
  resumeImpl(args);
  mScene->setClearColour(mpp::Colour::Black);
}

void StatePlay::teardown() {}

void StatePlay::resumeImpl(void* args) {
  if (!args) throw std::runtime_error("Missing application transition data");
  mTransitionData.userData = static_cast<applib::StateTransitionData*>(args)->userData;
}

void StatePlay::injectKeyInputImpl(wp::application::KeyEvent event,
                                   wp::application::Key key,
                                   wp::application::KeyModifiers) {
  if (event == wp::application::KeyEvent::Pressed &&
      key == wp::application::Key::Escape) {
    // Controller resumes with Play's identity and runs the unload sequence.
    throw wp::application::ReturnFromStateException(&mTransitionData);
  }
}

void StatePlay::renderImpl(mpp::RenderSystem* renderSystem,
                           mpp::ResourceManager*) {
  renderSystem->renderScene(mScene, mCamera, glm::vec2(0.0f), getName());
}

}  // namespace pf::application
