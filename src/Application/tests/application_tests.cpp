#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <applib/StateTransitionData.h>
#include <willpower/application/StateExceptions.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <core/World.h>
#include <core/Agent.h>
#include <cmath>

#include "Application.h"
#include "Map.h"
#include "DirectoryResourceLocation.h"

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

void checkPlayerKeys(wp::application::State& play, core::World& world) {
  using wp::application::Key;
  using wp::application::KeyEvent;
  auto key = [&](KeyEvent event, Key value) { play._injectKeyInput(event, value, {}); };
  auto x = [&](core::AgentId id) { return world.lookupAgent(id).entity->getGlobalPosition().x; };
  world.pauseSimulation();
  auto first = world.getPlayerControlledAgent();
  if (!first) first = world.createAgent("Keyboard player", 0, 0, 1.f);
  auto second = world.createAgent("Other player", 0, 0, 1.5f);
  require(world.setAgentBehaviourAssignment(second, core::PlayerControlledBehaviour, 1, {}) &&
              (world.lookupAgent(first).entity->isPlayerControlled() ||
               world.setAgentBehaviourAssignment(first, core::PlayerControlledBehaviour, 1, {})),
          "Built-in must be assignable without a behaviour registry");
  require(world.resumeSimulation(), "Cannot resume keyboard fixture");
  float start = x(first), other = x(second);
  auto speed = world.lookupAgent(first).entity->getWalkSpeed();
  key(KeyEvent::Pressed, Key::RightArrow); play._update(1.f);
  require(std::abs(x(first) - start - speed) < 0.0001f && x(second) == other,
          "App arrows must move only the first eligible Agent");
  key(KeyEvent::Pressed, Key::LeftArrow); play._update(1.f);
  require(std::abs(x(first) - start - speed) < 0.0001f, "Both arrows must cancel");
  key(KeyEvent::Released, Key::RightArrow); play._update(1.f);
  require(std::abs(x(first) - start) < 0.0001f, "Held left must reverse direction");
  key(KeyEvent::Released, Key::LeftArrow); play._update(1.f);
  require(std::abs(x(first) - start) < 0.0001f, "Released arrows must stop immediately");
  key(KeyEvent::Pressed, Key::RightArrow);
  play._suspend(true, true, true);
  play._suspend(false, false, false);
  play._update(1.f);
  require(std::abs(x(first) - start) < 0.0001f, "Suspending must clear held Player input");
  world.pauseSimulation();
  require(world.clearAgentBehaviourAssignment(first) && world.clearAgentBehaviourAssignment(second),
          "Cannot clear Player assignments");
  require(world.resumeSimulation(), "Cannot resume without Player");
  key(KeyEvent::Pressed, Key::RightArrow); play._update(1.f);
  require(!world.getPlayerControlledAgent() && std::abs(x(first) - start) < 0.0001f,
          "No eligible Agent must disable Player input");
  key(KeyEvent::Released, Key::RightArrow);
}

// Drive the actual factory-created states through the same exception/resume
// contract as Launcher, without entering their graphics-dependent scenes.
void checkFlow(std::array<wp::application::StateFactory*, 6> const& factories,
               wp::application::resourcesystem::ResourceManager& manager,
               bool exitInPlay = false) {
  std::shared_ptr<pf::application::Map> map;
  std::weak_ptr<void const> worldLifetime;
  std::unique_ptr<wp::application::State> controller(factories[0]->createState());
  require(controller->getName() == "Controller", "Controller must be initial state");
  // Enable update gates without _enter's graphics setup. Leaving every channel
  // unsuspended invokes only AppLib's no-op suspend hook.
  controller->_suspend(false, false, false);

  for (std::size_t index = 1; index < factories.size(); ++index) {
    bool moved = false;
    void* arguments = nullptr;
    try {
      controller->_update(0.0f);
    } catch (wp::application::SuspendAndMoveToStateException const& change) {
      require(change.getNextState() == factories[index]->getType(),
              "Controller selected the wrong next state");
      require(change.getEventsSuspended() && change.getUpdateSuspended() &&
                  change.getRenderSuspended(),
              "Controller must suspend while its child is active");
      require(change.getArguments() != nullptr, "Missing Controller transition data");
      arguments = change.getArguments();
      auto const& data = *static_cast<applib::StateTransitionData*>(arguments);
      requireNoMap(data);
      require(data.userData != nullptr, "Controller must own the resource lifecycle");
      controller->_suspend(true, true, true, change.getArguments());
      moved = true;
    }
    require(moved, "Controller must dispatch each lifecycle state");

    std::unique_ptr<wp::application::State> child(factories[index]->createState());
    require(child->getName() == factories[index]->getType(), "State name mismatch");
    child->_resume(arguments);
    require(!child->_imGuiActive(), "Lifecycle must not display a GUI");
    require(child->getDebuggingText().empty(), "Lifecycle must have no debug text");

    bool returned = false;
    try {
      if (child->getName() == "Play") {
        require(map && manager.isResourceLoaded(map) && map->getWorld(),
                "Map must remain loaded throughout Play");
        auto const tick = map->getWorld()->getSimulationTick();
        require(!map->getWorld()->isSimulationPaused() &&
                    map->getWorld()->getSimulationTimeScale() == 1.0,
                "Play must start the simulation at normal speed");
        child->_update(1.0f);
        require(map->getWorld()->getSimulationTick() == tick + 60,
                "Play must advance one second at normal speed");
        checkPlayerKeys(*child, *map->getWorld());
        auto const suspendTick = map->getWorld()->getSimulationTick();
        child->_suspend(true, true, true);
        child->_update(1.0f);
        require(map->getWorld()->getSimulationTick() == suspendTick,
                "Suspended Play must not update the World");
        child->_resume(arguments);
        if (exitInPlay) {
          child.reset();
          controller.reset(); // Same ordering as Launcher on an early exit.
          require(worldLifetime.expired() && !map->getWorld() &&
                      !manager.isResourceLoaded(map) && !manager.isResourceCreated(map),
                  "Early Controller destruction must release and destroy the Map");
          return;
        }
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
    if (child->getName() == "Load") {
      map = std::dynamic_pointer_cast<pf::application::Map>(manager.getResource("World", "World"));
      require(map && !map->getWorld(), "Load must scan but not create the Map");
    } else if (child->getName() == "MapLoad") {
      require(map->getWorld() && manager.isResourceLoaded(map),
              "MapLoad must create and load the World resource");
      require(map->getWorld()->getSimulationTick() == 0,
              "Loading must not advance the simulation");
      // Play must override a paused/non-normal runtime configuration on entry.
      map->getWorld()->pauseSimulation();
      map->getWorld()->setSimulationTimeScale(2.0);
      worldLifetime = map->getWorld()->getLifetimeToken();
    } else if (child->getName() == "MapUnload" || child->getName() == "Unload") {
      auto source = map->getDependentResource("World");
      require(worldLifetime.expired() && !map->getWorld() &&
                  !manager.isResourceLoaded(map) && !manager.isResourceCreated(map) &&
                  !manager.isResourceLoaded(source) && !manager.isResourceCreated(source),
              "Unloading must release both Map and its source data");
    }
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

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the staged resource directory");
    require(std::string_view(dllGetName()) == "Promethium Fermide",
            "Application name mismatch");
    require(dllSetArgument("Unknown", "true") != 0,
            "Unknown arguments must be rejected");
    require(!dllGetNextStateFactory(), "No factory before entry");

    constexpr std::array<std::string_view, 6> names{
        "Controller", "Load", "MapLoad", "Play", "MapUnload", "Unload"};
    for (int run = 0; run < 2; ++run) {
      wp::Logger logger;
      wp::application::resourcesystem::ResourceManager manager(nullptr, nullptr, nullptr, &logger);
      dllOnEntry(&logger, &manager, nullptr);
      manager.addResourceLocationFactory("Directory",
          [&logger](std::string const& path, std::string const& definition)
              -> wp::application::resourcesystem::ResourceLocation* {
            return new DirectoryResourceLocation(&logger, path, definition);
          });
      manager.addResourceLocation("Directory", argv[1], "Resources.yaml");
      std::array<wp::application::StateFactory*, 6> factories{};
      for (std::size_t index = 0; index < names.size(); ++index) {
        factories[index] = dllGetNextStateFactory();
        require(factories[index] && factories[index]->getType() == names[index],
                "Unexpected state factory or registration order");
      }
      require(!dllGetNextStateFactory(), "Only six state factories expected");
      require(!dllGetNextStateFactory(), "Enumeration must stay exhausted");
      checkFlow(factories, manager);
      checkFlow(factories, manager); // Recreate destroyed data with the same manager.
      checkFlow(factories, manager, true);
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
