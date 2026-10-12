#include <memory>
#include <vector>

#include "Application.h"
#include "StatePlay.h"
#include "StateController.h"
#include "StateLifecycle.h"

namespace {

template <class State>
class StateFactory final : public wp::application::StateFactory {
  wp::Logger* mLogger;

public:
  StateFactory(char const* name, wp::Logger* logger)
      : wp::application::StateFactory(name), mLogger(logger) {}

  wp::application::State* createState() override {
    auto state = std::make_unique<State>();
    state->setLogger(mLogger);
    return state.release();
  }
};

std::vector<std::unique_ptr<wp::application::StateFactory>> factories;
std::size_t nextFactory = 0;

}  // namespace

extern "C" {

char const* dllGetName() {
  return "Promethium Fermide";
}

int dllSetArgument(char const*, char const*) {
  // This bare-bones application has no configurable arguments.
  return 1;
}

wp::application::StateFactory* dllGetNextStateFactory() {
  if (nextFactory == factories.size()) return nullptr;
  return factories[nextFactory++].get();
}

void dllOnEntry(wp::Logger* logger,
                wp::application::resourcesystem::ResourceManager*,
                wp::application::AudioSystem*) {
  using namespace pf::application;
  factories.clear();
  nextFactory = 0;
  // Launcher uses the first registered factory as its initial state.
  factories.push_back(std::make_unique<StateFactory<StateController>>("Controller", logger));
  factories.push_back(std::make_unique<StateFactory<StateLoad>>("Load", logger));
  factories.push_back(std::make_unique<StateFactory<StateMapLoad>>("MapLoad", logger));
  factories.push_back(std::make_unique<StateFactory<StatePlay>>("Play", logger));
  factories.push_back(std::make_unique<StateFactory<StateMapUnload>>("MapUnload", logger));
  factories.push_back(std::make_unique<StateFactory<StateUnload>>("Unload", logger));
}

void dllOnExit() {
  factories.clear();
  nextFactory = 0;
}

}  // extern "C"
