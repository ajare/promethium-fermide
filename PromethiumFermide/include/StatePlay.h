#pragma once

#include <memory>
#include <applib/State.h>
#include "PlayDebug.h"

namespace core { class World; }
namespace game { class WorldRuntime; }

namespace pf::application {
namespace render { class WorldRenderer; }

// Use AppLib's scene lifecycle without StatePlay's map/player requirements.
class StatePlay final : public applib::State {
  std::unique_ptr<render::WorldRenderer> mRenderer;
  // Borrowed from Controller's MapResources. The runtime owns the World; this
  // state drives the runtime and never holds the World itself, so everything
  // Play reads - including what it hands the renderer - is the runtime's
  // const view (#970).
  game::WorldRuntime* mRuntime{nullptr};
  bool mLeftHeld{false}, mRightHeld{false}, mUpHeld{false}, mDownHeld{false};
  bool mCursorPressed[4]{false, false, false, false};
  bool mSpaceHeld{false}, mShiftHeld{false}, mF11Held{false}, mCrouchHeld{false};
  PlayDebug mDebug;
  void resetInput() {
    mLeftHeld = mRightHeld = mUpHeld = mDownHeld = mSpaceHeld = mShiftHeld = mF11Held = mCrouchHeld = false;
    for (auto& held : mCursorPressed) held = false;
  }
protected:
  void setup(wp::application::resourcesystem::ResourceManager*,
             mpp::RenderSystem*, mpp::ResourceManager*, void*) override;
  void teardown() override;
  void resumeImpl(void* args) override;
  void suspendImpl(void* args) override;
  void updateImpl(float elapsedSeconds) override;
  void injectKeyInputImpl(wp::application::KeyEvent event,
                          wp::application::Key key,
                          wp::application::KeyModifiers modifiers) override;
  void renderImpl(mpp::RenderSystem* renderSystem,
                  mpp::ResourceManager*) override;

public:
  StatePlay();
  ~StatePlay() override;
  bool isDebugModeEnabled() const { return mDebug.enabled(); }
  core::TraversalResourceId getDebugLift() const { return mDebug.lift(); }
};

}  // namespace pf::application
