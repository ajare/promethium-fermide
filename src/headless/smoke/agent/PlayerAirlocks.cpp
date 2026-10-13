#include "Checks.h"
#include "core/World.h"
#include "core/AirlockTransit.h"
#include "core/MobilityProfile.h"
#include "core/Graph.h"
#include <cmath>

namespace {
using smoke::require;
void journey(bool reverse, uint32_t width) {
 core::World world("Manual Airlock", 20, 2);
 auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
 auto right = world.addRoom("Right", 0, 0, 4 + width, 4, 1);
 auto index = world.addAirlock(0, 0, 4, width, 1.f);
 auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
 auto source = reverse ? right : left, target = reverse ? left : right;
 auto point = world.lookupInteractionPoint(chamber->getControl(reverse ? 1 : 0)).entity;
 world.finishBuild(); world.pauseSimulation();
 auto id = world.createAgent("Player", source, 0, point->getPosition().x - world.getSector(source)->getPosition().x);
 require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Airlock Player");
 require(world.resumeSimulation(), "Cannot resume Airlock fixture");
 auto actor = world.lookupAgent(id).entity;
 auto step = [&] {
  world.update(core::World::getFixedTimestep());
  require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Both Airlock Doors must never be open together");
 };
 auto wait = [&](int ticks) { for (int i = 0; i < ticks; ++i) step(); };
 int direction = reverse ? -1 : 1;
 world.requestPlayerDoorAction();
 for (int i = 0; i < 600 && !chamber->getDoor(reverse ? 1 : 0)->isOpen(); ++i) step();
 require(chamber->getDoor(reverse ? 1 : 0)->isOpen(), "Up must call the Airlock entrance");
 require(actor->getSector()->getIndex() == source, "Calling must not approach or enter Airlock");
 world.setPlayerMovementDirection(direction);
 for (int i = 0; i < 300 && actor->getSector()->getIndex() != index; ++i) step();
 world.setPlayerMovementDirection(0);
 require(actor->getSector()->getIndex() == index, "Left/Right must manually enter the open Airlock");
 auto stopped = actor->getGlobalPosition();
 world.pauseSimulation();
 auto revoked = world.addAccessPermission("Exit landing after admission");
 require(world.setLocationPermissionRequirement(target, {revoked}), "Cannot tighten committed exit permission");
 world.update(1.f);
 require(actor->getGlobalPosition().distanceTo(stopped) < .0001f, "Pause must retain Airlock occupancy and position");
 world.resumeSimulation();
 for (int i = 0; i < 4000 && !chamber->getDoor(reverse ? 0 : 1)->isOpen(); ++i) step();
 require(chamber->getDoor(reverse ? 0 : 1)->isOpen() && chamber->isCycleComplete(), "Closed-door cycle must open committed opposite exit: width=" + std::to_string(width) + ", reverse=" + std::to_string(reverse) + ", doors=" + std::to_string((int)world.getSimulationSnapshot().airlocks.front().doors[0]) + "/" + std::to_string((int)world.getSimulationSnapshot().airlocks.front().doors[1]) + ", cycle=" + std::to_string(chamber->getRemainingCycleSeconds()) + ", occupants=" + std::to_string(world.getSimulationSnapshot().airlocks.front().occupants.size()) + ", crossings=" + std::to_string(world.getSimulationSnapshot().airlocks.front().crossings.size()));
 wait(1000);
 require(chamber->getDoor(reverse ? 0 : 1)->isOpen(), "Stopped Player must retain a usable exit beyond ordinary Door timeout");
 require(actor->getSector()->getIndex() == index && actor->getGlobalPosition().distanceTo(stopped) < .0001f,
   "Release must stop; cycle completion must never align or automatically eject Player");
 require(world.getSimulationSnapshot().airlocks.front().occupants.size() == 1, "Stopped Player must retain shared capacity");
 world.setPlayerMovementDirection(-direction); wait(60); world.setPlayerMovementDirection(0);
 require(actor->getSector()->getIndex() == index, "The entrance must not become an exit that bypasses cycling");
 world.setPlayerMovementDirection(direction);
 for (int i = 0; i < 1200 && actor->getSector()->getIndex() != target; ++i) step();
 world.setPlayerMovementDirection(0);
 require(actor->getSector()->getIndex() == target, "Player must manually exit through opposite open Door: width=" + std::to_string(width) + ", reverse=" + std::to_string(reverse) + ", x=" + std::to_string(actor->getGlobalPosition().x) + ", doorX=" + std::to_string(chamber->getDoor(reverse ? 0 : 1)->getPosition().x) + ", doorW=" + std::to_string(chamber->getDoor(reverse ? 0 : 1)->getSize().x));
 require(world.getSimulationSnapshot().airlocks.front().occupants.empty(), "Manual exit must release capacity");
}
void partialOpening(bool reverse) {
 core::World world("Physical Airlock Doors", 12, 2);
 auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
 auto right = world.addRoom("Right", 0, 0, 5, 4, 1);
 auto index = world.addAirlock(0, 0, 4, 1, 1.f);
 auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
 auto source = reverse ? right : left, target = reverse ? left : right;
 float x = world.lookupInteractionPoint(chamber->getControl(reverse ? 1 : 0)).entity->getPosition().x
   + (reverse ? .05f : -.05f);
 world.finishBuild(); world.pauseSimulation();
 auto id = world.createAgent("Player", source, 0, x - world.getSector(source)->getPosition().x);
 world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {});
 require(world.setAgentIndividualHeightModifier(id, .8f), "Cannot shorten partial-opening Player fixture");
 world.resumeSimulation();
 auto actor = world.lookupAgent(id).entity;
 auto step = [&] {
  world.update(core::World::getFixedTimestep());
  require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Partial passage must retain Airlock interlock");
  for (auto side : {0, 1}) {
   core::Vector2 min, max; chamber->getDoor(side)->getCurrentShape(min, max);
   auto pos = actor->getGlobalPosition(); float half = actor->getPoseEnvelope(core::Pose::Standing)->x * .5f;
   if (pos.x + half > min.x + .0001f && pos.x - half < max.x - .0001f)
    require(pos.y + actor->getStandingHeight() <= min.y + .0001f,
      "Player must never intersect a partially open Airlock Door bounding box: reverse=" + std::to_string(reverse)
      + ", x=" + std::to_string(pos.x) + ", height=" + std::to_string(actor->getStandingHeight())
      + ", leaf=" + std::to_string(min.y) + ", side=" + std::to_string(side));
  }
 };
 world.requestPlayerDoorAction(); world.setPlayerMovementDirection(reverse ? -1 : 1);
 for (int i = 0; i < 2400 && actor->getSector()->getIndex() != index; ++i) step();
 require(actor->getSector()->getIndex() == index && chamber->getDoor(reverse ? 1 : 0)->isOpening(),
   "Player must complete Airlock entry before full opening when its bounding box clears: x=" + std::to_string(actor->getGlobalPosition().x)
   + ", sector=" + std::to_string(actor->getSector()->getIndex()) + ", pct=" + std::to_string(chamber->getDoor(reverse ? 1 : 0)->getOpenPercentage()));
 for (int i = 0; i < 4800 && actor->getSector()->getIndex() != target; ++i) step();
 world.setPlayerMovementDirection(0);
 require(actor->getSector()->getIndex() == target && chamber->getDoor(reverse ? 0 : 1)->isOpening(),
   "Player must complete committed exit before full opening without bypassing the cycle");
 require(world.getSimulationSnapshot().airlocks.front().occupants.empty(), "Partial-open exit must release shared capacity");
}
void restrictionsAndRetracing() {
 for (int scenario = 0; scenario < 5; ++scenario) {
  core::World world("Airlock entry restrictions", 12, 2);
  auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
  auto right = world.addRoom("Right", 0, 0, 6, 4, 1);
  auto index = world.addAirlock(0, 0, 4, 2, 1.f);
  auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
  auto point = world.lookupInteractionPoint(chamber->getControl(0)).entity;
  world.finishBuild(); world.pauseSimulation();
  auto id = world.createAgent("Player", left, 0, point->getPosition().x);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign restricted Player");
  auto actor = world.lookupAgent(id).entity;
  if (scenario == 0) {
   auto profile = actor->getEffectiveMobilityProfile().value;
   profile.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
   world.setAgentIndividualMobilityProfile(id, profile);
  }
  if (scenario == 1) {
   auto key = world.addAccessPermission("Exit landing");
   require(world.setLocationPermissionRequirement(right, {key}), "Cannot protect exit");
  }
  if (scenario == 2) require(world.setRoomHeightScale(right, .2f), "Cannot lower exit ceiling");
  world.resumeSimulation(); world.requestPlayerDoorAction();
  auto step = [&] {
   world.update(core::World::getFixedTimestep());
   require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Restrictions must not bypass interlocks");
  };
  for (int i = 0; i < 1200 && !chamber->getDoor(0)->isOpen(); ++i) step();
  require(chamber->getDoor(0)->isOpen(), "Restricted fixture must open entrance");
  world.setPlayerMovementDirection(1);
  if (scenario < 3) {
   for (int i = 0; i < 300; ++i) step();
   require(actor->getSector()->getIndex() == left && world.getSimulationSnapshot().airlocks.front().occupants.empty(),
     "Entry must respect Mobility, opposite landing permissions and Standing clearance");
  } else if (scenario == 4) {
   for (int i = 0; i < 600 && actor->getSector()->getIndex() != index; ++i) step();
   world.setPlayerMovementDirection(0);
   require(actor->getSector()->getIndex() == index, "Reset fixture must retain an admitted occupant");
   world.resetSimulation(); actor = world.lookupAgent(id).entity;
   chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
   require(actor && actor->getSector()->getIndex() == left
     && world.getSimulationSnapshot().airlocks.front().occupants.empty()
     && world.getSimulationSnapshot().airlocks.front().reservations.empty()
     && world.getSimulationSnapshot().airlocks.front().crossings.empty()
     && chamber->getDoor(0)->isClosed() && chamber->getDoor(1)->isClosed(),
     "Reset must restore authored position and clear manual Airlock ownership");
  } else {
   bool partial = false;
   for (int i = 0; i < 300 && !partial; ++i) {
    step(); partial = !world.getSimulationSnapshot().airlocks.front().crossings.empty();
   }
   require(partial && actor->getSector()->getIndex() == left, "Fixture must reach an admitted partial crossing");
   world.setPlayerMovementDirection(0); auto position = actor->getGlobalPosition();
   world.pauseSimulation(); world.update(1.f); world.resumeSimulation();
   for (int i = 0; i < 800; ++i) step();
   require(actor->getGlobalPosition().distanceTo(position) < .0001f && chamber->getDoor(0)->isOpen(),
     "Stopped admitted crossing must keep its boundary safe beyond the admission window");
   world.setPlayerMovementDirection(-1);
   for (int i = 0; i < 120 && !world.getSimulationSnapshot().airlocks.front().crossings.empty(); ++i) step();
   require(world.getSimulationSnapshot().airlocks.front().crossings.empty()
     && world.getSimulationSnapshot().airlocks.front().reservations.empty() && actor->getSector()->getIndex() == left,
     "Retracing entrance must surrender crossing and provisional capacity without entering");
  }
 }
}
void sharedBatch() {
 core::World world("Player and autonomous Airlock batch", 12, 2);
 auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
 auto right = world.addRoom("Right", 0, 0, 6, 4, 1);
 auto index = world.addAirlock(0, 0, 4, 2, 1.f);
 auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
 float x = world.lookupInteractionPoint(chamber->getControl(0)).entity->getPosition().x;
 world.finishBuild(); world.pauseSimulation();
 auto id = world.createAgent("Player", left, 0, x);
 world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}); world.resumeSimulation();
 auto actor = world.lookupAgent(id).entity;
 world.requestPlayerDoorAction();
 auto step = [&] { world.update(core::World::getFixedTimestep()); require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Shared batch violated interlock"); };
 for (int i = 0; i < 1200 && !chamber->getDoor(0)->isOpen(); ++i) step();
 world.setPlayerMovementDirection(1);
 for (int i = 0; i < 600 && actor->getGlobalPosition().x < 5.45f; ++i) step();
 world.setPlayerMovementDirection(0);
 require(actor->getSector()->getIndex() == index, "Player must occupy interior before sharing batch");
 auto otherId = world.createAgent("Autonomous", left, 0, 3.f);
 auto other = world.lookupAgent(otherId).entity;
 auto target = world.getGraph()->getClosestVertexInSector(world.getSector(right).get(), {7.5f, 0.f});
 auto path = world.getGraph()->calculatePath(other, target);
 require(bool(path), "Cannot route autonomous batch member"); other->setPath(path, true);
 bool shared = false;
 for (int i = 0; i < 4000 && other->getSector()->getIndex() != right; ++i) {
  step(); shared = shared || world.getSimulationSnapshot().airlocks.front().occupants.size() == 2;
 }
 require(shared && other->getSector()->getIndex() == right && actor->getSector()->getIndex() == index,
   "Autonomous passenger must share the batch and exit without ejecting Player");
 world.setPlayerMovementDirection(1);
 for (int i = 0; i < 600 && actor->getSector()->getIndex() != right; ++i) step();
 require(actor->getSector()->getIndex() == right && world.getSimulationSnapshot().airlocks.front().occupants.empty(), "Last manual exit must drain shared capacity");
}
}
namespace agent_smoke {
void registerPlayerAirlocks(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlAirlocks", [](smoke::Context const&) {
  for (uint32_t width : {1u, 2u, 5u}) for (bool reverse : {false, true}) journey(reverse, width);
  partialOpening(false); partialOpening(true);
  restrictionsAndRetracing(); sharedBatch();
 }});
}
}
