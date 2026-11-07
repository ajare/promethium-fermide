#include <memory>
#include <vector>
#include <type_traits>

#include <willpower/application/resourcesystem/ResourceManager.h>

#include "Application.h"
#include "StatePlay.h"
#include "StateController.h"
#include "StateLifecycle.h"
#include "ApplicationResources.h"

namespace {

template <class State>
class StateFactory final : public wp::application::StateFactory {
  wp::Logger* mLogger;
  wp::application::resourcesystem::ResourceManager* mResourceMgr;

public:
  StateFactory(char const* name, wp::Logger* logger,
               wp::application::resourcesystem::ResourceManager* resourceMgr = nullptr)
      : wp::application::StateFactory(name), mLogger(logger), mResourceMgr(resourceMgr) {}

  wp::application::State* createState() override {
    std::unique_ptr<State> state;
    if constexpr (std::is_same_v<State, pf::application::StateController>) {
      state = std::make_unique<State>(mResourceMgr, mLogger);
    } else {
      state = std::make_unique<State>();
    }
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
                wp::application::resourcesystem::ResourceManager* resourceMgr,
                wp::application::AudioSystem*) {
  using namespace pf::application;
  if (resourceMgr) {
    registerApplicationResourceTypes(*resourceMgr);
  }
  factories.clear();
  nextFactory = 0;
  // Launcher uses the first registered factory as its initial state.
  factories.push_back(std::make_unique<StateFactory<StateController>>("Controller", logger, resourceMgr));
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
