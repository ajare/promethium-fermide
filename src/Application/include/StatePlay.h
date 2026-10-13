#pragma once

#include <memory>
#include <applib/State.h>

namespace pf::application {
namespace render { class WorldRenderer; }

// Use AppLib's scene lifecycle without StatePlay's map/player requirements.
class StatePlay final : public applib::State {
  std::unique_ptr<render::WorldRenderer> mRenderer;
protected:
  void setup(wp::application::resourcesystem::ResourceManager*,
             mpp::RenderSystem*, mpp::ResourceManager*, void*) override;
  void teardown() override;
  void resumeImpl(void* args) override;
  void injectKeyInputImpl(wp::application::KeyEvent event,
                          wp::application::Key key,
                          wp::application::KeyModifiers modifiers) override;
  void renderImpl(mpp::RenderSystem* renderSystem,
                  mpp::ResourceManager*) override;

public:
  StatePlay();
  ~StatePlay() override;
};

}  // namespace pf::application
