#include <stdexcept>
#include <willpower/application/StateExceptions.h>

#include "StatePlay.h"
#include "Map.h"
#include "ApplicationResources.h"
#include "WorldRenderer.h"
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace pf::application {

StatePlay::StatePlay() : applib::State("Play") {}
StatePlay::~StatePlay() = default;

void StatePlay::setup(wp::application::resourcesystem::ResourceManager* resources,
                      mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResources,
                      void* args) {
  resumeImpl(args);
  mScene->setClearColour(mpp::Colour::Black);
  auto map = std::dynamic_pointer_cast<Map>(resources->getResource("World", "World"));
  if (!map || !map->getWorld()) throw std::runtime_error("Play requires the loaded World Map");
  mRenderer = std::make_unique<render::WorldRenderer>(*resources, *renderSystem,
      *renderResources, mScene, map->getWorldResource()->world());
}

void StatePlay::teardown() { mRenderer.reset(); }

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
  mScene->setViewport(0, 0, renderSystem->getWindowWidth(), renderSystem->getWindowHeight());
  // Batches stay hidden from the normal scene pass: the Application renderer
  // submits them explicitly in command order with each aperture's scissor.
  renderSystem->renderScene(mScene, mCamera, glm::vec2(0.0f), getName());
  if (mRenderer) mRenderer->render();
}

}  // namespace pf::application
