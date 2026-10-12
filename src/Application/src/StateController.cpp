#include <stdexcept>

#include "StateController.h"

namespace pf::application {

StateController::StateController() : applib::StateController("Load") {}

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
  // No resources, maps, or model data to transfer yet.
}

}  // namespace pf::application
