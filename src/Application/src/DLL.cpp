#include <memory>

#include "Application.h"
#include "StatePlay.h"

namespace {

class StatePlayFactory final : public wp::application::StateFactory {
  wp::Logger* mLogger;

public:
  explicit StatePlayFactory(wp::Logger* logger)
      : wp::application::StateFactory("Play"), mLogger(logger) {}

  wp::application::State* createState() override {
    auto state = std::make_unique<pf::application::StatePlay>();
    state->setLogger(mLogger);
    return state.release();
  }
};

std::unique_ptr<StatePlayFactory> playFactory;
bool factoryReturned = false;

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
  if (factoryReturned || !playFactory) {
    return nullptr;
  }
  factoryReturned = true;
  return playFactory.get();
}

void dllOnEntry(wp::Logger* logger,
                wp::application::resourcesystem::ResourceManager*,
                wp::application::AudioSystem*) {
  playFactory = std::make_unique<StatePlayFactory>(logger);
  factoryReturned = false;
}

void dllOnExit() {
  playFactory.reset();
  factoryReturned = false;
}

}  // extern "C"
