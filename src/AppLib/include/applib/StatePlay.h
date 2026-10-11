#pragma once

#include <vector>
#include <tuple>

#include <willpower/application/StateFactory.h>
#include <willpower/application/InputStateManager.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

#include <willpower/collide/Simulation.h>

#include <willpower/viz/AccelerationGridRenderer.h>
#include <willpower/viz/DynamicQuadRenderer.h>

#include "Platform.h"
#include "State.h"
#include "EntityManager.h"
#include "EntityHandler.h"
#include "ScreenFxManager.h"

namespace applib {

class APPLIB_API StatePlay : public State {
protected:
  enum class RenderOrder {
    Background = -2,
    Map = -1,
    Entities = 0,
    MapDebug
  };

private:
  wp::application::InputStateManager* mInputStateMgr;

  ScreenFxManager* mScreenFxMgr;

  std::vector<std::string> mActiveInputStates;

  float mMouseDeltaX, mMouseDeltaY;

  bool mUseDebugCamera;

  glm::vec2 mDebugCameraPos;

  mpp::CameraPtr mDebugCamera;

protected:
  EntityManager* mEntityMgr;

  wp::application::resourcesystem::ResourcePtr mMap;

  std::unique_ptr<wp::viz::GeometryMeshRenderer> mMapRenderer;

  std::unique_ptr<wp::collide::Simulation> mMapCollisionSim;

  std::map<std::string, std::tuple<wp::viz::Renderer*, int, bool>> mAdditionalRenderers;

private:
  virtual void setupEntityFacades();

  virtual void setupEntities();

  virtual void setupMapRenderer(StateTransitionData* transitionData);

  void setupMapCollisionSim(StateTransitionData* transitionData);

  void setupAdditionalRenderers(mpp::ResourceManager* renderResourceMgr);

  virtual std::map<std::string, std::tuple<wp::viz::Renderer*, int, bool>> createAdditionalRenderers(mpp::ResourceManager* renderResourceMgr);

  void destroyAdditionalRenderers();

  virtual void setInitialMapRenderParams(wp::viz::GeometryMeshRenderParams* params);

  virtual void registerInput() = 0;

  virtual void createGameObjects(wp::application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args);

  virtual void destroyGameObjects();

  void injectKeyInputImpl(wp::application::KeyEvent evt, wp::application::Key key, wp::application::KeyModifiers modifiers) override;

  void injectMouseButtonInputImpl(wp::application::MouseButtonEvent evt, wp::application::MouseButton mouseButton, wp::application::KeyModifiers modifiers) override;

  void injectMouseMotionInputImpl(float positionX, float positionY) override;

  void resyncMouseInputImpl() override;

  mpp::CameraPtr getActiveCamera() const;

  glm::vec2 getPlayerPosition() const;

protected:
  void createEntityFacade(
      std::string const& facadeType,
      std::vector<int> const& types,
      EntityFacadeRenderOptions const& options,
      size_t initialSize);

  Entity const& getPlayerEntity() const;

  void useDebugCamera(bool use);

  bool usingDebugCamera() const;

  wp::BoundingBox getViewBounds() const;

  wp::Vector2 getViewCentreWorldPosition() const;

  wp::Vector2 getViewOffsetWorldPosition() const;

  wp::Vector2 getMouseScreenPosition() const;

  wp::Vector2 getMouseScreenDelta() const;

  wp::Vector2 getMouseWorldPosition() const;

  virtual void updatePreInput(float frameTime);

  virtual void updatePreEntities(float frameTime);

  virtual void updatePostEntities(float frameTime);

  virtual void updatePreRenderers(float frameTime);

  void createRenderers(mpp::ResourceManager* renderResourceMgr, StateTransitionData* transitionData);

  void destroyRenderers();

  void createInput();

  void destroyInput();

  void updateInput(float frameTime);

  void createScreenFxManagement();

  void destroyScreenFxManagement();

  void updateScreenFxManagement(float frameTime);

  void createEntityManagement();

  void destroyEntityManagement();

  void updateEntityManagement(float frameTime);

  void updateRenderers(float frameTime);

  virtual void updateCamera(float frameTime);

  void createEntity(int type, wp::Vector2 const& position, float angle, bool addImmediately = false);

  virtual void setupScene();

  void setup(wp::application::resourcesystem::ResourceManager* resourceMgr, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args = nullptr) override;

  void teardown() override;

  void enterImpl(wp::application::resourcesystem::ResourceManager* resourceMgr, wp::application::AudioSystem* audioSystem, mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, void* args = nullptr) override;

  void exitImpl();

  void suspendImpl(void* args = nullptr);

  void resumeImpl(void* args) override;

  void registerInputState(std::string const& name,
                          std::vector<wp::application::Key> const& keysPressed,
                          std::vector<wp::application::Key> const& keysReleased,
                          std::vector<wp::application::Key> const& keysDown,
                          std::vector<wp::application::MouseButton> const& buttonsPressed,
                          std::vector<wp::application::MouseButton> const& buttonsReleased,
                          std::vector<wp::application::MouseButton> const& buttonsDown,
                          bool mouseWheelUp,
                          bool mouseWheelDown,
                          uint32_t keyModifiers,
                          bool disableInGui = false);

  void unregisterInputState(std::string const& name);

  virtual void updateActions(std::vector<std::string> const& activeStates, float frameTime);

  void updateImpl(float frameTime) override;

  void renderImpl(mpp::RenderSystem* renderSystem, mpp::ResourceManager* resourceMgr) override;

public:
  StatePlay();
};

}  // namespace applib