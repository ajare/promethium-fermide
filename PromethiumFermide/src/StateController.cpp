#include <stdexcept>

#include "StateController.h"

namespace pf::application {

StateController::StateController(
    wp::application::resourcesystem::ResourceManager* resourceMgr, wp::Logger* logger)
    : applib::StateController("Load") {
  if (resourceMgr) mResources = std::make_unique<MapResources>(*resourceMgr, logger);
  mTransitionData.userData = mResources.get();
}

void StateController::teardown() {
  mResources.reset();
  mTransitionData.userData = nullptr;
}

std::string StateController::getNextStateName(
    std::string const& previous, applib::StateTransitionData*) {
  if (previous == "Load") return "MapLoad";
  if (previous == "MapLoad") return "Play";
  if (previous == "Play") return "MapUnload";
  if (previous == "MapUnload") return "Unload";
  if (previous == "Unload") return "";
  throw std::runtime_error("Unknown application state: " + previous);
}

void StateController::updateTransitionData(
    std::string const&, std::string const&, applib::StateTransitionData*) {
  // The Controller-owned resource guard is shared through userData. No map
  // renderer, collision simulation, or model data is needed by Play.
}

}  // namespace pf::application
