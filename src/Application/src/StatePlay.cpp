#include <stdexcept>
#include <willpower/application/StateExceptions.h>

#include "StatePlay.h"
#include "Map.h"
#include "MapResources.h"
#include <core/World.h>
#include <core/Defines.h>
#include "ApplicationResources.h"
#include "WorldRenderer.h"
#include "RenderSettings.h"
#include <algorithm>
#include <cmath>
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

void StatePlay::teardown() {
  if (mWorld) mWorld->clearPlayerInput();
  resetInput();
  mRenderer.reset();
  mDebug = {};
  mWorld = nullptr;
}

void StatePlay::resumeImpl(void* args) {
  if (!args) throw std::runtime_error("Missing application transition data");
  mTransitionData.userData = static_cast<applib::StateTransitionData*>(args)->userData;
  auto* resources = static_cast<MapResources*>(mTransitionData.userData);
  auto* map = resources ? resources->getMap() : nullptr;
  mWorld = map ? map->getWorld() : nullptr;
  if (!mWorld) throw std::runtime_error("Play requires the loaded World Map");
  resetInput();
  mDebug = {};
  mWorld->clearPlayerInput();
  mWorld->setSimulationTimeScale(1.0);
  if (!mWorld->resumeSimulation()) throw std::runtime_error("Play cannot resume the World");
}

void StatePlay::suspendImpl(void*) {
  resetInput();
  if (mWorld) mWorld->clearPlayerInput();
}

void StatePlay::updateImpl(float elapsedSeconds) {
  if (mWorld) {
    mWorld->setPlayerMovementDirection(mShiftHeld ? 0 : int(mRightHeld) - int(mLeftHeld));
    mWorld->setPlayerStairSelection(!mShiftHeld && mUpHeld, !mShiftHeld && mDownHeld);
    mWorld->update(elapsedSeconds);
    mDebug.observe(*mWorld);
  }
}

void StatePlay::injectKeyInputImpl(wp::application::KeyEvent event,
                                   wp::application::Key key,
                                   wp::application::KeyModifiers modifiers) {
  using Key = wp::application::Key;
  bool pressed = event == wp::application::KeyEvent::Pressed;
  if (key == Key::F11) {
    if (pressed && !mF11Held && mWorld) mDebug.toggle(*mWorld);
    mF11Held = pressed;
    return;
  }
  mShiftHeld = (modifiers & wp::application::KeyModifiers::Shift) != 0;
  if (key == Key::LeftShift || key == Key::RightShift) mShiftHeld = pressed;
  int cursor = key == Key::LeftArrow ? 0 : key == Key::RightArrow ? 1
    : key == Key::UpArrow ? 2 : key == Key::DownArrow ? 3 : -1;
  if (mShiftHeld) {
    mLeftHeld = mRightHeld = mUpHeld = mDownHeld = false;
    if (mWorld) { mWorld->setPlayerMovementDirection(0); mWorld->setPlayerStairSelection(false, false); }
  }
  if (cursor >= 0) {
    bool fresh = pressed && !mCursorPressed[cursor];
    mCursorPressed[cursor] = pressed;
    if (mShiftHeld) {
      if (fresh && mWorld) mWorld->browsePlayerTransportStop(cursor == 1 || cursor == 2 ? 1 : -1, cursor < 2);
      return;
    }
    if (pressed && !fresh) return;
  }
  if (key == Key::Space) {
    if (pressed && !mSpaceHeld && mWorld) mWorld->confirmPlayerTransportStop();
    mSpaceHeld = pressed;
    return;
  }
  if (key == wp::application::Key::LeftArrow)
    mLeftHeld = event == wp::application::KeyEvent::Pressed;
  else if (key == wp::application::Key::RightArrow)
    mRightHeld = event == wp::application::KeyEvent::Pressed;
  else if (key == wp::application::Key::UpArrow) {
    bool const pressed = event == wp::application::KeyEvent::Pressed;
    if (pressed && !mUpHeld && mWorld) mWorld->requestPlayerDoorAction();
    mUpHeld = pressed;
  }
  else if (key == wp::application::Key::DownArrow)
    mDownHeld = event == wp::application::KeyEvent::Pressed;
  if (event == wp::application::KeyEvent::Pressed &&
      key == wp::application::Key::Escape) {
    resetInput();
    if (mWorld) mWorld->clearPlayerInput();
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
  if (!mDebug.enabled() || !mWorld || !renderSystem->getWindowWidth() || !renderSystem->getWindowHeight()) return;
  renderSystem->pushProjectionMatrix();
  renderSystem->pushModelMatrix();
  renderSystem->resetTransform();
  try {
    renderSystem->renderText("DEBUG [F11] - press a Lift Button for live variables", 9, 9, mpp::Colour::Black);
    renderSystem->renderText("DEBUG [F11] - press a Lift Button for live variables", 8, 8, mpp::Colour::White);
    if (auto readout = liftDebugReadout(*mWorld, mDebug.lift())) {
      auto const& view = render::gRenderSettings;
      float x = readout->anchor.x * CORE_CELL_WIDTH_PIXELS * view.worldZoom + view.worldViewportX + view.xOffset;
      float y = view.worldViewportY + view.worldViewportHeight - readout->anchor.y * CORE_LEVEL_HEIGHT_PIXELS * view.worldZoom - view.yOffset;
      size_t columns = 0; for (auto const& line : readout->lines) columns = std::max(columns, line.size());
      x = std::clamp(x, 8.f, std::max(8.f, float(renderSystem->getWindowWidth()) - float(columns * 8) - 8.f));
      y = std::clamp(y, 32.f, std::max(32.f, float(renderSystem->getWindowHeight()) - float(readout->lines.size() * 16) - 8.f));
      renderSystem->renderText(readout->lines, int(std::round(x)) + 1, int(std::round(y)) + 1, mpp::Colour::Black);
      renderSystem->renderText(readout->lines, int(std::round(x)), int(std::round(y)), mpp::Colour::White);
    }
  } catch (...) {
    renderSystem->popModelMatrix(); renderSystem->popProjectionMatrix(); throw;
  }
  renderSystem->popModelMatrix(); renderSystem->popProjectionMatrix();
}

}  // namespace pf::application
