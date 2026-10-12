#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <applib/StateTransitionData.h>
#include <willpower/application/StateExceptions.h>

#include "Application.h"

namespace {
void require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}

void requireNoMap(applib::StateTransitionData const& data) {
  auto const& map = data.mapData;
  require(map.nextMapName.empty() && !map.prevMap.map && !map.nextMap.map &&
              !map.prevMap.mapRenderer && !map.nextMap.mapRenderer &&
              !map.prevMap.mapCollisionSim && !map.nextMap.mapCollisionSim,
          "Lifecycle must not populate map data");
}

// Drive the actual factory-created states through the same exception/resume
// contract as Launcher, without entering their graphics-dependent scenes.
void checkFlow(std::array<wp::application::StateFactory*, 6> const& factories) {
  std::unique_ptr<wp::application::State> controller(factories[0]->createState());
  require(controller->getName() == "Controller", "Controller must be initial state");
  // Enable update gates without _enter's graphics setup. Leaving every channel
  // unsuspended invokes only AppLib's no-op suspend hook.
  controller->_suspend(false, false, false);

  for (std::size_t index = 1; index < factories.size(); ++index) {
    bool moved = false;
    try {
      controller->_update(0.0f);
    } catch (wp::application::SuspendAndMoveToStateException const& change) {
      require(change.getNextState() == factories[index]->getType(),
              "Controller selected the wrong next state");
      require(change.getEventsSuspended() && change.getUpdateSuspended() &&
                  change.getRenderSuspended(),
              "Controller must suspend while its child is active");
      require(change.getArguments() != nullptr, "Missing Controller transition data");
      requireNoMap(*static_cast<applib::StateTransitionData*>(change.getArguments()));
      controller->_suspend(true, true, true, change.getArguments());
      moved = true;
    }
    require(moved, "Controller must dispatch each lifecycle state");

    std::unique_ptr<wp::application::State> child(factories[index]->createState());
    require(child->getName() == factories[index]->getType(), "State name mismatch");
    child->_suspend(false, false, false);
    require(!child->_imGuiActive(), "Lifecycle must not display a GUI");
    require(child->getDebuggingText().empty(), "Lifecycle must have no debug text");

    bool returned = false;
    try {
      if (child->getName() == "Play") {
        child->_update(0.0f); // Play waits for Escape, unlike the loading placeholders.
        child->_injectKeyInput(wp::application::KeyEvent::Released,
                               wp::application::Key::Escape, {});
        child->_injectKeyInput(wp::application::KeyEvent::Pressed,
                               wp::application::Key::Space, {});
        child->_injectKeyInput(wp::application::KeyEvent::Pressed,
                               wp::application::Key::Escape, {});
      } else {
        child->_update(0.0f);
      }
    } catch (wp::application::ReturnFromStateException const& change) {
      require(change.getArguments() != nullptr, "Child must return transition data");
      auto const& data = *static_cast<applib::StateTransitionData*>(change.getArguments());
      require(data.prevStateName == child->getName(), "Wrong returning state identity");
      requireNoMap(data);
      // Resume before destroying the child: its return data belongs to it.
      controller->_resume(change.getArguments());
      returned = true;
    }
    require(returned, "Child must return to Controller");
  }

  bool finished = false;
  try {
    controller->_update(0.0f);
  } catch (wp::application::ReturnFromStateException const&) {
    finished = true;
  }
  require(finished, "Controller must exit after Unload");

  applib::StateTransitionData invalid;
  invalid.prevStateName = "MapTransition";
  bool rejected = false;
  try {
    controller->_resume(&invalid);
  } catch (std::runtime_error const&) {
    rejected = true;
  }
  require(rejected, "There must be no MapTransition flow");
}
}

int main() {
  try {
    require(std::string_view(dllGetName()) == "Promethium Fermide",
            "Application name mismatch");
    require(dllSetArgument("Unknown", "true") != 0,
            "Unknown arguments must be rejected");
    require(!dllGetNextStateFactory(), "No factory before entry");

    constexpr std::array<std::string_view, 6> names{
        "Controller", "Load", "MapLoad", "Play", "MapUnload", "Unload"};
    for (int run = 0; run < 2; ++run) {
      dllOnEntry(nullptr, nullptr, nullptr);
      std::array<wp::application::StateFactory*, 6> factories{};
      for (std::size_t index = 0; index < names.size(); ++index) {
        factories[index] = dllGetNextStateFactory();
        require(factories[index] && factories[index]->getType() == names[index],
                "Unexpected state factory or registration order");
      }
      require(!dllGetNextStateFactory(), "Only six state factories expected");
      require(!dllGetNextStateFactory(), "Enumeration must stay exhausted");
      checkFlow(factories);
      dllOnExit();
      require(!dllGetNextStateFactory(), "No factory after exit");
      dllOnExit();
    }
    return 0;
  } catch (std::exception const& error) {
    dllOnExit();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
