#include <mpp/helper/OrthoCamera.h>

#include <willpower/application/StateExceptions.h>
#include <willpower/application/ServiceLocator.h>
#include <willpower/application/ApplicationSettings.h>

#include "StatePlay.h"
#include "ModelInstance.h"

namespace applib {

using namespace std;
using namespace wp;

StatePlay::StatePlay()
    : State("Play"), mInputStateMgr(nullptr), mEntityMgr(nullptr), mScreenFxMgr(nullptr), mMouseDeltaX(0.0f), mMouseDeltaY(0.0f), mUseDebugCamera(false) {
}

Entity const& StatePlay::getPlayerEntity() const {
  return mEntityMgr->getPlayerEntity();
}

void StatePlay::useDebugCamera(bool use) {
  if (use && !mUseDebugCamera) {
    mDebugCameraPos = getPlayerPosition();
  }

  mUseDebugCamera = use;
}

bool StatePlay::usingDebugCamera() const {
  return mUseDebugCamera;
}

BoundingBox StatePlay::getViewBounds() const {
  auto const& cameraPos = getActiveCamera()->getPosition();

  Vector2 viewPos(cameraPos.x, cameraPos.z);

  const Vector2 viewSize = getWindowSize();
  return BoundingBox(viewPos - viewSize / 2, viewSize);
}

Vector2 StatePlay::getViewCentreWorldPosition() const {
  auto const& camViewPos = getActiveCamera()->getPosition();
  return Vector2(camViewPos.x, camViewPos.z);
}

Vector2 StatePlay::getViewOffsetWorldPosition() const {
  Vector2 screenSize = getWindowSize();
  return getViewCentreWorldPosition() - screenSize / 2;
}

wp::Vector2 StatePlay::getMouseScreenPosition() const {
  float mouseX, mouseY;
  mInputStateMgr->mousePosition(&mouseX, &mouseY);

  return {mouseX, mouseY};
}

wp::Vector2 StatePlay::getMouseScreenDelta() const {
  return {mMouseDeltaX, mMouseDeltaY};
}

wp::Vector2 StatePlay::getMouseWorldPosition() const {
  float mouseX, mouseY;
  mInputStateMgr->mousePosition(&mouseX, &mouseY);

  auto worldOffset = getViewOffsetWorldPosition();
  return {
      worldOffset.x + mouseX,
      worldOffset.y + (mwRenderSystem->getWindowHeight() - mouseY)};
}

void StatePlay::createEntity(int type, Vector2 const& position, float angle, bool addImmediately) {
  mEntityMgr->createEntity(type, position, angle, addImmediately);
}

void StatePlay::createInput() {
  destroyInput();
  mInputStateMgr = new application::InputStateManager();
}

void StatePlay::destroyInput() {
  delete mInputStateMgr;
  mInputStateMgr = nullptr;
}

void StatePlay::updateInput(float frameTime) {
  WP_UNUSED(frameTime);

  mActiveInputStates = mInputStateMgr->process(_imGuiCapturesInput(), &mMouseDeltaX, &mMouseDeltaY);

  if (usingDebugCamera()) {
    const float debugCameraSpeed{500};

    for (auto const& state : mActiveInputStates) {
      if (state == "Up") {
        mDebugCameraPos.y += debugCameraSpeed * frameTime;
      } else if (state == "Down") {
        mDebugCameraPos.y -= debugCameraSpeed * frameTime;
      } else if (state == "Left") {
        mDebugCameraPos.x -= debugCameraSpeed * frameTime;
      } else if (state == "Right") {
        mDebugCameraPos.x += debugCameraSpeed * frameTime;
      }
    }
  } else {
    auto mouseScreenPos = getMouseScreenPosition();
    auto mouseWorldPos = getMouseWorldPosition();

    ModelInstance::entityHandler()->setActiveInputStates(mActiveInputStates, mouseScreenPos.x, mouseScreenPos.y, mMouseDeltaX, mMouseDeltaY, mouseWorldPos);
  }

  updateActions(mActiveInputStates, frameTime);
}

void StatePlay::createScreenFxManagement() {
  mScreenFxMgr = new ScreenFxManager(mwRenderResourceMgr);
}

void StatePlay::destroyScreenFxManagement() {
  delete mScreenFxMgr;
  mScreenFxMgr = nullptr;
}

void StatePlay::updateScreenFxManagement(float frameTime) {
  mScreenFxMgr->update(frameTime);
}

void StatePlay::createEntityManagement() {
  Entity::_resetIdGenerator();

  mEntityMgr = new EntityManager(mwRenderSystem, mwRenderResourceMgr);

  setupEntityFacades();
  setupEntities();
}

void StatePlay::destroyEntityManagement() {
  delete mEntityMgr;
  mEntityMgr = nullptr;
}

void StatePlay::updateEntityManagement(float frameTime) {
  mEntityMgr->updateEntities(!usingDebugCamera(), frameTime);
}

void StatePlay::setupEntityFacades() {
  // Designed to be implemented by subclasses
}

void StatePlay::setupEntities() {
  // Designed to be implemented by subclasses
}

void StatePlay::createEntityFacade(
    string const& facadeType,
    vector<int> const& types,
    EntityFacadeRenderOptions const& options,
    size_t initialSize) {
  mEntityMgr->createFacade(
      facadeType,
      mScene,
      types,
      options,
      (int)RenderOrder::Entities,
      initialSize);
}

void StatePlay::setInitialMapRenderParams(viz::GeometryMeshRenderParams* params) {
  params->setRender(true);
  params->setRenderBackground(true);
  params->setRenderVertices(true);
  params->setVertexColour(mpp::Colour::Red);
  params->setRenderTriangulation(false);
  params->setRenderPolygonEdges(true);
  params->setPolygonEdgeColour(mpp::Colour::Blue);
  params->setRenderBorder(false);
  params->setRenderEdgeDirections(false);
  params->setRenderEdgeNormals(false);
  params->setBackgroundColour(mpp::Colour::White);
}

map<string, tuple<wp::viz::Renderer*, int, bool>> StatePlay::createAdditionalRenderers(mpp::ResourceManager* renderResourceMgr) {
  return {};
}

void StatePlay::destroyAdditionalRenderers() {
  for (auto item : mAdditionalRenderers) {
    auto const& [name, details] = item;
    auto renderer = get<0>(details);
    delete renderer;
  }

  mAdditionalRenderers.clear();
}

void StatePlay::setupMapRenderer(StateTransitionData* transitionData) {
  mMapRenderer = transitionData->mapData.nextMap.mapRenderer ? move(transitionData->mapData.nextMap.mapRenderer) : nullptr;
  if (mMapRenderer) {
    mMapRenderer->addToScene(mScene, (int)RenderOrder::Map);

    auto mapRenderParams = static_cast<wp::viz::GeometryMeshRenderParams*>(mMapRenderer->getParams().get());
    setInitialMapRenderParams(mapRenderParams);
    mMapRenderer->updateRenderParams();
  }
}

void StatePlay::setupMapCollisionSim(StateTransitionData* transitionData) {
  mMapCollisionSim = transitionData->mapData.nextMap.mapCollisionSim ? move(transitionData->mapData.nextMap.mapCollisionSim) : nullptr;
  ModelInstance::get()->collisionSim = mMapCollisionSim.get();
}

void StatePlay::setupAdditionalRenderers(mpp::ResourceManager* renderResourceMgr) {
  mAdditionalRenderers = createAdditionalRenderers(renderResourceMgr);
  for (auto item : mAdditionalRenderers) {
    auto const& [name, details] = item;
    auto const& [renderer, order, show] = details;

    renderer->build(mwRenderSystem, mwRenderResourceMgr);
    renderer->addToScene(mScene, order);

    renderer->setVisible(show);
  }
}

void StatePlay::updatePreInput(float frameTime) {
  // Designed to be implemented by subclasses
}

void StatePlay::updatePreEntities(float frameTime) {
  // Designed to be implemented by subclasses
}

void StatePlay::updatePostEntities(float frameTime) {
  // Designed to be implemented by subclasses
}

void StatePlay::updatePreRenderers(float frameTime) {
  // Designed to be implemented by subclasses
}

void StatePlay::createRenderers(mpp::ResourceManager* renderResourceMgr, StateTransitionData* transitionData) {
  // Map renderer is created in MapLoad state so just set up here
  setupMapRenderer(transitionData);

  // Additional renderers are created by subclass and then
  // managed by the base class, so set up here
  setupAdditionalRenderers(renderResourceMgr);
}

void StatePlay::destroyRenderers() {
  // Map renderer is destroyed in MapUnload state
  destroyAdditionalRenderers();
}

void StatePlay::updateRenderers(float frameTime) {
  auto viewBounds = getViewBounds();

  // Map
  if (mMapRenderer) {
    mMapRenderer->update(viewBounds, frameTime);
  }

  // Entities
  mEntityMgr->updateRenderers(viewBounds, frameTime);

  // User-supplied
  for (auto item : mAdditionalRenderers) {
    auto const& [name, details] = item;
    auto renderer = get<0>(details);

    renderer->update(viewBounds, frameTime);
  }
}

void StatePlay::updateCamera(float frameTime) {
  if (usingDebugCamera()) {
    // Set debug camera position
    static_cast<mpp::helper::OrthoCamera*>(mDebugCamera.get())->setPosition(mDebugCameraPos);
  } else {
    // Set player camera position
    static_cast<mpp::helper::OrthoCamera*>(mCamera.get())->setPosition(getPlayerPosition());
  }
}

void StatePlay::setupScene() {
  mScene->setClearColour(mpp::Colour::Black);
}

void StatePlay::createGameObjects(application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args) {
  // Designed to be implemented by subclasses
}

void StatePlay::setup(application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args) {
  WP_UNUSED(resourceMgr);

  auto transitionData = static_cast<StateTransitionData*>(args);

  // Set up objects to pass to next state
  mTransitionData.mapData.prevMap.map = transitionData->mapData.nextMap.map;
  mMap = transitionData->mapData.nextMap.map;

  createInput();
  createScreenFxManagement();
  createEntityManagement();

  setupMapCollisionSim(transitionData);

  createRenderers(renderResourceMgr, transitionData);

  setupScene();
  loadAllReferencedResources();

  // Set up input
  registerInput();

  // For subclasses
  createGameObjects(resourceMgr, renderSystem, renderResourceMgr, args);
}

void StatePlay::destroyGameObjects() {
  // Designed to be implemented by subclasses
}

void StatePlay::teardown() {
  // For subclasses
  destroyGameObjects();
}

void StatePlay::enterImpl(wp::application::resourcesystem::ResourceManager* resourceMgr, wp::application::AudioSystem* audioSystem, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args) {
  State::enterImpl(resourceMgr, audioSystem, renderSystem, renderResourceMgr, args);

  mDebugCamera = make_shared<mpp::helper::OrthoCamera>(
      glm::vec2(0.0f, 0.0f),
      renderSystem->getWindowWidth(),
      renderSystem->getWindowHeight());
}

void StatePlay::exitImpl() {
  State::exitImpl();

  mDebugCamera.reset();

  destroyRenderers();
  destroyEntityManagement();
  destroyScreenFxManagement();
  destroyInput();
}

void StatePlay::suspendImpl(void* args) {
  WP_UNUSED(args);
}

void StatePlay::resumeImpl(void* args) {
  WP_UNUSED(args);
}

void StatePlay::injectKeyInputImpl(application::KeyEvent evt, application::Key key, application::KeyModifiers modifiers) {
  mInputStateMgr->injectKeyInput(evt, key, modifiers);
}

void StatePlay::injectMouseButtonInputImpl(application::MouseButtonEvent evt, application::MouseButton mouseButton, application::KeyModifiers modifiers) {
  mInputStateMgr->injectMouseInput(evt, mouseButton, modifiers);
}

void StatePlay::injectMouseMotionInputImpl(float positionX, float positionY) {
  mInputStateMgr->setMousePosition(positionX, positionY);
}

void StatePlay::resyncMouseInputImpl() {
  // Motion can be withheld before the state has its input, and after it has
  // given it up again.
  if (mInputStateMgr) {
    mInputStateMgr->resyncMousePosition();
  }
}

mpp::CameraPtr StatePlay::getActiveCamera() const {
  return mUseDebugCamera ? mDebugCamera : mCamera;
}

glm::vec2 StatePlay::getPlayerPosition() const {
  auto playerEnt = getPlayerEntity();

  if (ModelInstance::entityHandler()->entityHasComponent<PhysicalStats>(playerEnt)) {
    auto const& physicalStats = ModelInstance::entityHandler()->getEntityComponent<PhysicalStats>(playerEnt);
    return glm::vec2(physicalStats.position.x, physicalStats.position.y);
  } else {
    throw Exception("Player entity does not have a position property.");
  }
}

void StatePlay::registerInputState(string const& name,
                                   vector<wp::application::Key> const& keysPressed,
                                   vector<wp::application::Key> const& keysReleased,
                                   vector<wp::application::Key> const& keysDown,
                                   vector<wp::application::MouseButton> const& buttonsPressed,
                                   vector<wp::application::MouseButton> const& buttonsReleased,
                                   vector<wp::application::MouseButton> const& buttonsDown,
                                   bool mouseWheelUp,
                                   bool mouseWheelDown,
                                   uint32_t keyModifiers,
                                   bool disableInGui) {
  mInputStateMgr->registerState(name,
                                keysPressed,
                                keysReleased,
                                keysDown,
                                buttonsPressed,
                                buttonsReleased,
                                buttonsDown,
                                mouseWheelUp,
                                mouseWheelDown,
                                keyModifiers,
                                disableInGui);
}

void StatePlay::unregisterInputState(string const& name) {
  mInputStateMgr->unregisterState(name);
}

void StatePlay::updateActions(vector<string> const& activeStates, float frameTime) {
  // Designed to be implemented by subclasses
}

void StatePlay::updateImpl(float frameTime) {
  updatePreInput(frameTime);
  updateInput(frameTime);
  updatePreEntities(frameTime);
  updateEntityManagement(frameTime);
  updatePostEntities(frameTime);
  updateCamera(frameTime);
  updatePreRenderers(frameTime);
  updateScreenFxManagement(frameTime);
  updateRenderers(frameTime);
}

void StatePlay::renderImpl(mpp::RenderSystem* renderSystem, mpp::ResourceManager* resourceMgr) {
  WP_UNUSED(resourceMgr);

  // Screen FX setup
  mScreenFxMgr->preRender(getViewCentreWorldPosition());

  // Render scene
  auto viewOffset = getViewOffsetWorldPosition();
  renderSystem->renderScene(
      mScene,
      getActiveCamera(),
      glm::vec2(viewOffset.x, viewOffset.y),
      getName());

  // Render post-effects
  mScreenFxMgr->postRender(renderSystem);
}

}  // namespace applib