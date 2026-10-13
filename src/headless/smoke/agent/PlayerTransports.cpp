#include "Checks.h"
#include "core/World.h"
#include "core/DoorSectorObject.h"
#include "core/Lift.h"
#include "core/MobilityProfile.h"
#include <algorithm>
#include <cmath>

namespace
{
 using smoke::require;
 core::TraversalResourceSnapshot vehicle(core::World& world, core::TraversalResourceId id)
 {
  auto snapshot = world.getSimulationSnapshot();
  auto found = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
   [&](auto const& value) { return value.id == id; });
  require(found != snapshot.traversalResources.end(), "Missing vehicle");
  return *found;
 }
 void tick(core::World& world, float seconds) { for (int i = 0; i < int(seconds / core::World::getFixedTimestep()); ++i) world.update(core::World::getFixedTimestep()); }
 void enclosed(bool shuttle)
 {
  core::World world("Manual Player transport", 20, 6);
  auto lower = world.addRoom("Origin", 0, 0, 0, 6, 1);
  if (shuttle) world.addRoom("Destination", 0, 0, 10, 6, 1);
  else { world.addRoom("Middle", 0, 2, 0, 6, 1); world.addRoom("Top", 0, 4, 0, 6, 1); }
  core::TraversalResourceId resource;
  core::World::CreateDoorResult firstDoor;
  uint32_t transit;
  if (shuttle) {
   core::World::CreateShuttleOptions options{1, 4, {0, 10}, 0};
   options.minimumDwellSeconds = 5.f; options.maximumBoardingSeconds = 20.f;
   auto created = world.addShuttle(1, 0, 0, 19, options);
   resource = created.traversalResource; firstDoor = created.doors.front(); transit = created.shuttle.sector->getIndex();
  } else {
   core::World::CreateLiftOptions options; options.cellsWide = 2; options.stopOffsets = {0, 2, 4};
   options.minimumDwellSeconds = 5.f; options.maximumBoardingSeconds = 20.f;
   auto created = world.addLift(1, 0, 2, options);
   resource = created.traversalResource; firstDoor = created.doors.front(); transit = created.lift.sector->getIndex();
  }
  auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(firstDoor.door.sector->getObject(firstDoor.door.index))->getDoor();
  auto point = world.lookupInteractionPoint(firstDoor.controls[0].interactionPoint).entity;
  world.finishBuild(); world.pauseSimulation();
  auto id = world.createAgent("Player", lower, 0, point->getPosition().x - world.getSector(lower)->getPosition().x);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Player");
  auto actor = world.lookupAgent(id).entity;

  require(world.resumeSimulation(), "Cannot resume");
  world.requestPlayerDoorAction(); tick(world, 1.f);
  require(!world.getPlayerTransportSelector(), "Selector must be absent outside");
  float centre = door->getPosition().x + door->getSize().x * .5f;
  int direction = actor->getGlobalPosition().x < centre ? 1 : -1;
  world.setPlayerMovementDirection(direction);
  for (int i = 0; i < 120 && direction * (centre - actor->getGlobalPosition().x) > .05f; ++i) world.update(core::World::getFixedTimestep());
  world.setPlayerMovementDirection(0);
  for (int i = 0; i < 180 && !door->isOpen(); ++i) world.update(core::World::getFixedTimestep());
  float x = actor->getGlobalPosition().x;
  world.requestPlayerDoorAction(); tick(world, 1.f);
  require(actor->getSector()->getIndex() == transit, std::string(shuttle ? "Shuttle" : "Lift") + " Up must board an open vehicle: phase=" + std::to_string((int)vehicle(world, resource).liftStopPhase) + ", door=" + std::to_string(door->isOpen()) + ", x=" + std::to_string(actor->getGlobalPosition().x));
  require(std::abs(actor->getGlobalPosition().x - x) < .001f, "Boarding must preserve physical X, not align to a slot");
  auto selector = world.getPlayerTransportSelector();
  require(selector && selector->focus == 1 && selector->horizontal == shuttle, "Boarded Player must see the contextual selector");
  world.confirmPlayerTransportStop(); world.confirmPlayerTransportStop();
  require(vehicle(world, resource).liftStopRequestOwnerCounts[1] == 1, "Confirmation must be idempotent");
  if (!shuttle) { world.browsePlayerTransportStop(1, false); world.confirmPlayerTransportStop(); require(vehicle(world, resource).liftStopRequestOwnerCounts[2] == 1, "Multiple service requests must coexist"); }
  require(shuttle ? world.setShuttleBroken(resource, true) : world.setLiftBroken(resource, true), "Cannot break occupied vehicle");
  require(!world.getPlayerTransportSelector(), "Broken operation must hide selector");
  auto frozen = actor->getGlobalPosition(); tick(world, .2f);
  require(actor->getGlobalPosition().distanceTo(frozen) < .001f, "Broken vehicle must retain physical passenger position");
  world.requestPlayerDoorAction(); tick(world, .5f);
  require(actor->getSector()->getIndex() == lower, "Up must manually leave an already-open Broken vehicle");
  require(vehicle(world, resource).liftStopRequestOwnerCounts[1] == 1, "Manual requests must survive enclosed-vehicle exit");
  world.requestPlayerDoorAction(); tick(world, .2f);
  require(actor->getSector()->getIndex() == lower, "Broken open vehicle must refuse new boarders");
  require(shuttle ? world.setShuttleBroken(resource, false) : world.setLiftBroken(resource, false), "Cannot restore vehicle");
  world.requestPlayerDoorAction(); tick(world, .5f);
  require(actor->getSector()->getIndex() == transit, "Player must be able to reboard without a journey destination");
  world.pauseSimulation(); require(!world.getPlayerTransportSelector(), "Pause must hide selector"); world.resumeSimulation();
  bool walkedDuringTravel = false;
  for (int i = 0; i < 75; ++i) {
   world.update(1.f);
   auto moving = vehicle(world, resource);
   if (moving.liftMoving && !walkedDuringTravel) {
    world.setPlayerStairSelection(false, true); tick(world, .02f);
    require(actor->getPose() == core::Pose::Crouching, "Down must crouch inside a moving enclosed vehicle");
    float relative = actor->getGlobalPosition().x - (shuttle ? vehicle(world, resource).liftPosition : 0.f);
    world.setPlayerMovementDirection(1); tick(world, .1f);
    require(actor->getPose() == core::Pose::Crawling, "Down plus horizontal movement must crawl inside the vehicle");
    world.setPlayerMovementDirection(0); world.setPlayerStairSelection(false, false); tick(world, .02f);
    require(actor->getPose() == core::Pose::Standing, "Releasing Down inside the vehicle must restore Standing");
    float after = actor->getGlobalPosition().x - (shuttle ? vehicle(world, resource).liftPosition : 0.f);
    require(after > relative + .01f, "Ordinary movement must work while vehicle travels without automatic slot alignment");
    walkedDuringTravel = true;
   }
  }
  require(walkedDuringTravel, "Fixture must exercise walking during travel");
  require(actor->getSector()->getIndex() == transit, "Served destinations must never automatically disembark Player");
  require(vehicle(world, resource).liftStopRequestOwnerCounts[1] == 0, std::string(shuttle ? "Shuttle" : "Lift") + " served manual request must clear: current=" + std::to_string(vehicle(world, resource).liftCurrentStop) + ", phase=" + std::to_string((int)vehicle(world, resource).liftStopPhase));
  world.setPlayerMovementDirection(1); auto before = actor->getGlobalPosition(); tick(world, .2f); world.setPlayerMovementDirection(0);
  require(actor->getGlobalPosition().x > before.x, "Passenger must retain ordinary horizontal movement");
 }
 void calledLanding()
 {
  core::World world("Called one-cell Lift at Level 1", 16, 3);
  world.addCorridor(0, 12, 4);
  auto room = world.addRoom("Level 1 landing", 0, 1, 10, 5, 1);
  core::World::CreateLiftOptions options; options.cellsWide = 1; options.stopOffsets = {0, 1};
  auto created = world.addLift(1, 0, 14, options);
  auto const& landing = created.doors[1];
  auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(landing.door.sector->getObject(landing.door.index))->getDoor();
  auto point = world.lookupInteractionPoint(landing.controls[0].interactionPoint).entity;
  world.finishBuild(); world.pauseSimulation();
  auto id = world.createAgent("Player", room, 0, point->getPosition().x - world.getSector(room)->getPosition().x);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign called-Lift Player");
  require(world.resumeSimulation(), "Cannot resume called-Lift fixture");
  auto actor = world.lookupAgent(id).entity;
  world.requestPlayerDoorAction(); world.update(core::World::getFixedTimestep());
  float centre = door->getPosition().x + door->getSize().x * .5f;
  int direction = actor->getGlobalPosition().x < centre ? 1 : -1;
  world.setPlayerMovementDirection(direction);
  for (int i = 0; i < 180 && direction * (centre - actor->getGlobalPosition().x) > .02f; ++i) world.update(core::World::getFixedTimestep());
  world.setPlayerMovementDirection(0);
  for (int i = 0; i < 600 && !door->isOpen(); ++i) world.update(core::World::getFixedTimestep());
  require(door->isOpen(), "Called Lift must arrive and open the Level 1 Door");
  auto state = vehicle(world, created.traversalResource);
  float x = actor->getGlobalPosition().x;
  world.requestPlayerDoorAction(); tick(world, .5f);
  require(actor->getSector()->getIndex() == created.lift.sector->getIndex(),
    "Up must board the called Lift at Level 1: phase=" + std::to_string((int)state.liftStopPhase)
      + ", x=" + std::to_string(x) + ", y=" + std::to_string(actor->getGlobalPosition().y));
  require(world.getPlayerTransportSelector().has_value(), "Boarding must leave time to select a Stop");
  tick(world, 1.f);
  require(world.getPlayerTransportSelector().has_value(), "The short minimum dwell must not cut off manual Stop selection");
  world.confirmPlayerTransportStop();
  require(vehicle(world, created.traversalResource).liftStopRequestOwnerCounts[0] == 1, "Player must be able to request a visit after entering");
 }
 void platform()
 {
  core::World world("Manual Platform lift", 12, 5);
  auto room = world.addRoom("Room", 0, 0, 0, 12, 5);
  for (uint32_t x = 0; x < 12; ++x) { world.addSectorWalkway(room, 2, x); world.addSectorWalkway(room, 4, x); }
  core::World::CreateLiftOptions options; options.cellsWide = 1; options.stopOffsets = {0, 2, 4};
  options.platformStopDurationSeconds = 10.f;
  auto created = world.addSectorPlatformLift(room, 0, 4, options);
  auto point = world.lookupInteractionPoint(created.buttons.front().interactionPoint).entity;
  world.finishBuild(); world.pauseSimulation();
  auto id = world.createAgent("Player", room, 0, point->getPosition().x);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign platform Player");
  require(world.resumeSimulation(), "Cannot resume platform");
  world.requestPlayerDoorAction(); tick(world, .5f);
  auto actor = world.lookupAgent(id).entity;
  auto profile = actor->getEffectiveMobilityProfile().value;
  auto forbidden = profile; forbidden.set(core::TraversalKind::PlatformLift, core::MobilityUse::CannotUse);
  world.pauseSimulation(); world.setAgentIndividualMobilityProfile(id, forbidden); world.resumeSimulation();
  world.setPlayerMovementDirection(actor->getGlobalPosition().x < 4.5f ? 1 : -1); tick(world, .4f);
  require(!world.getPlayerTransportSelector() && vehicle(world, created.traversalResource).occupantCount == 0,
    "Forbidden Platform lift entry must not fall through to ordinary Floor movement");
  world.pauseSimulation(); world.setAgentIndividualMobilityProfile(id, profile); world.resumeSimulation();
  world.setPlayerMovementDirection(actor->getGlobalPosition().x < 4.5f ? 1 : -1);
  for (int i = 0; i < 240 && !world.getPlayerTransportSelector(); ++i) world.update(core::World::getFixedTimestep());
  world.setPlayerMovementDirection(0);
  auto selector = world.getPlayerTransportSelector();
  require(selector && !selector->horizontal, "Left/Right must manually board a stopped Platform lift");
  world.pauseSimulation(); world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None); world.resumeSimulation();
  require(world.getPlayerTransportSelector() && world.getPlayerTransportSelector()->focus == ~0u,
    "Without selection capability or shared demand, no new destination may be selected");
  world.pauseSimulation(); world.setAgentIndividualObjectUsage(id, core::ObjectUsage::Arms);
  auto permission = world.addAccessPermission("Select middle Stop");
  require(world.setLiftDestinationPermissionRequirement(room, 1, {permission}, nullptr, created.lift.index), "Cannot protect Stop");
  world.resumeSimulation();
  require(world.getPlayerTransportSelector()->focus == 2, "Selector must skip unauthorized destinations");
  world.pauseSimulation(); world.grantAgentAccessPermission(id, permission); world.resumeSimulation();
  world.confirmPlayerTransportStop();
  auto start = actor->getGlobalPosition();
  world.setPlayerMovementDirection(start.x < 4.5f ? -1 : 1);
  tick(world, 1.f); world.setPlayerMovementDirection(0);
  require(!world.getPlayerTransportSelector(), "Horizontal movement must manually leave Platform lift");
  require(vehicle(world, created.traversalResource).liftStopRequestOwnerCounts[1] == 1, "Stop request must survive leaving");
  for (int second = 0; second < 30; ++second) world.update(1.f);
  require(vehicle(world, created.traversalResource).liftStopRequestOwnerCounts[1] == 0, "Unoccupied platform must still serve the requested Stop");
  require(std::abs(actor->getGlobalPosition().y) < .001f, "A departed Player must not be carried by the platform");
  world.pauseSimulation(); world.resetSimulation();
  require(vehicle(world, created.traversalResource).liftScheduledStops.empty(), "Reset must discard runtime service requests");
  require(!world.getPlayerTransportSelector(), "Reset must not retain selector or crossing state");
 }
}
namespace agent_smoke
{
 void registerPlayerTransports(std::vector<smoke::Check>& checks)
 {
  checks.push_back({"playerControlLiftCalledLanding", [](smoke::Context const&) { calledLanding(); }});
  checks.push_back({"playerControlManualTransports", [](smoke::Context const&) { enclosed(false); enclosed(true); platform(); }});
 }
}
