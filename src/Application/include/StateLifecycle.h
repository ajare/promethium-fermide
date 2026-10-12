#pragma once

#include <applib/State.h>

namespace pf::application {

// AppLib's loading states perform resource/map work. These placeholders only
// reuse its scene lifecycle and return to Controller on their first update.
class StateLifecycle : public applib::State {
protected:
  explicit StateLifecycle(std::string const& name);
  void setup(wp::application::resourcesystem::ResourceManager*,
             mpp::RenderSystem*, mpp::ResourceManager*, void*) override;
  void teardown() override;
  void updateImpl(float) override;
  void renderImpl(mpp::RenderSystem*, mpp::ResourceManager*) override;
};

class StateLoad final : public StateLifecycle {
public:
  StateLoad() : StateLifecycle("Load") {}
};

class StateMapLoad final : public StateLifecycle {
public:
  StateMapLoad() : StateLifecycle("MapLoad") {}
};

class StateMapUnload final : public StateLifecycle {
public:
  StateMapUnload() : StateLifecycle("MapUnload") {}
};

class StateUnload final : public StateLifecycle {
public:
  StateUnload() : StateLifecycle("Unload") {}
};

}  // namespace pf::application
