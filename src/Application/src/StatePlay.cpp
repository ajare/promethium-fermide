#include <willpower/application/StateExceptions.h>

#include "StatePlay.h"

namespace pf::application {

StatePlay::StatePlay() : applib::State("Play") {}

void StatePlay::setup(wp::application::resourcesystem::ResourceManager*,
                      mpp::RenderSystem*, mpp::ResourceManager*, void*) {
  mScene->setClearColour(mpp::Colour::Black);
}

void StatePlay::teardown() {}

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
