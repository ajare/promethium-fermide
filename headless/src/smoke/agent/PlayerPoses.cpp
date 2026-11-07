#include "Checks.h"
#include "core/World.h"
#include "core/AgentType.h"
#include <fstream>
#include <sstream>
#include <cmath>

namespace {
using smoke::require;
using core::Pose;
void input(core::World& world, int direction, bool down, float seconds = .1f) {
 if (world.isTraversalTopologyDirty()) require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
 require(world.resumeSimulation(), "Cannot resume Player pose fixture");
 world.setPlayerMovementDirection(direction); world.setPlayerCrouch(down); world.update(seconds);
}
core::AgentId player(core::World& world, uint32_t room, float x, std::string type = "Human") {
 auto id = world.createAgent(type, "Player", room, 0, x);
 require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign pose Player"); return id;
}
void controls() {
 core::World world("Player poses", 12, 2); auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
 world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 2.f);
 auto actor = [&] { return world.lookupAgent(id).entity; };
 input(world, 0, true); require(actor()->getPose() == Pose::Crouching && actor()->getGlobalPosition().x == 2.f, "C must crouch without movement");
 input(world, 1, true); require(actor()->getPose() == Pose::Crawling && actor()->getGlobalPosition().x > 2.f, "C+Right must crawl");
 auto x = actor()->getGlobalPosition().x; input(world, -1, true);
 require(actor()->getPose() == Pose::Crawling && actor()->getGlobalPosition().x < x, "C+Left must crawl in reverse");
 input(world, 0, true); require(actor()->getPose() == Pose::Crouching, "Stopping horizontal input while C remains held must crouch");
 input(world, 0, false); require(actor()->getPose() == Pose::Standing, "Releasing C must stand when it fits");
 input(world, 0, true); world.pauseSimulation(); world.update(1.f);
 require(actor()->getPose() == Pose::Crouching, "Pause must freeze manual posture");
 world.resumeSimulation(); world.update(.1f); require(actor()->getPose() == Pose::Standing, "Resume must not revive cleared C input");
 input(world, 0, true); world.pauseSimulation(); world.setAgentActive(id, false); world.resumeSimulation();
 world.setPlayerCrouch(true); world.update(.1f); require(actor()->getPose() == Pose::Crouching, "Inactive Agent must not receive pose updates");
 world.pauseSimulation(); world.setAgentActive(id, true); world.resetSimulation();
 require(actor()->getPose() == Pose::Standing, "Reset must discard manual pose ownership");
 input(world, 0, true); world.pauseSimulation(); world.clearAgentBehaviourAssignment(id); world.resumeSimulation(); world.update(.1f);
 require(actor()->getPose() == Pose::Standing, "Behaviour handoff must release manual pose ownership");
}
void capabilities(smoke::Context const& context) {
 std::ifstream file(context.fixture("resources/test-worlds/human.agent.lua")); std::ostringstream buffer; buffer << file.rdbuf();
 require(bool(file), "Cannot read Human pose fixture");
 for (int mode = 0; mode < 4; ++mode) {
  std::istringstream lines(buffer.str()); std::string source, line;
  while (std::getline(lines, line)) {
   if (!(mode & 1) && (line.find("crouching =") != std::string::npos || line.find("pose = \"crouching\"") != std::string::npos)) continue;
   if (!(mode & 2) && (line.find("crawling =") != std::string::npos || line.find("pose = \"crawling\"") != std::string::npos)) continue;
   for (size_t at; (at = line.find("\"Human\"")) != std::string::npos;) line.replace(at, 7, "\"PoseTest\"");
   auto at = line.find("pose = \"crawling\", speed_ratio = 1");
   if (at != std::string::npos) line.replace(at, std::string("pose = \"crawling\", speed_ratio = 1").size(), "pose = \"crawling\", speed_ratio = 0.4");
   source += line + '\n';
  }
  core::World world("Pose capabilities", 12, 2); auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
  std::string diagnostic; require(world.attachAgentType("pose-test.agent.lua", source, &diagnostic), diagnostic);
  world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 2.f, "PoseTest");
  auto actor = world.lookupAgent(id).entity;
  input(world, 0, true); auto expected = mode & 1 ? Pose::Crouching : Pose::Standing;
  require(actor->getPose() == expected, "Unsupported Crouching must be a no-op");
  auto x = actor->getGlobalPosition().x; auto speed = actor->getWalkSpeed(); input(world, 1, true, 1.f);
  require(actor->getPose() == (mode & 2 ? Pose::Crawling : expected), "Unsupported Crawling must leave the existing pose unchanged");
  require(std::abs(actor->getGlobalPosition().x - x - speed * (mode & 2 ? .4f : 1.f)) < .001f, "Manual motion must use declared context pose speed");
 }
}
void clearance() {
 core::World world("Manual low clearance", 12, 2);
 auto tall = world.addRoom("Tall", 0, 0, 0, 4, 1); auto low = world.addRoom("Low", 0, 0, 4, 4, 1);
 world.addRoom("Tall exit", 0, 0, 8, 4, 1); world.finishBuild(); world.pauseSimulation();
 require(world.setRoomHeightScale(low, .4f), "Cannot lower test Room");
 world.removeLocationWall(tall, 0, CORE_SIDE_RIGHT); world.removeLocationWall(low, 0, CORE_SIDE_RIGHT);
 auto id = player(world, tall, 3.f);
 input(world, 1, true, 4.f); auto actor = world.lookupAgent(id).entity;
 require(actor->getSector()->getIndex() == low && actor->getPose() == Pose::Crawling, "Explicit Crawl must respect and enter supported low space");
 input(world, 0, false); require(actor->getPose() == Pose::Crawling, "Release must not stand into a low ceiling: height=" + std::to_string(actor->getStandingHeight())
  + ", ceiling=" + std::to_string(actor->getSector()->getEffectiveTopLevelHeight()) + ", pose=" + std::to_string((int)actor->getPose()));
 input(world, 1, false, 8.f); require(actor->getPose() == Pose::Standing && actor->getGlobalPosition().x > 8.2f, "Standing must resume only after the whole body clears low space");
 input(world, 1, true, 20.f); require(actor->getGlobalPosition().x <= 11.8f + .001f && actor->getPose() == Pose::Crawling, "Crawl must not tunnel through boundary walls");
}
void climbing() {
 auto definition = core::bundledHumanAgentType();
 auto replace = [&](std::string from, std::string to) {
  auto at = definition.source.find(from); require(at != std::string::npos, "Missing pose declaration");
  definition.source.replace(at, from.size(), to);
 };
 replace("type_id = \"Human\"", "type_id = \"TransitPose\"");
 replace("pose = \"crouching\", speed_ratio = 1", "pose = \"crouching\", speed_ratio = 0.6");
 replace("pose = \"crawling\", speed_ratio = 1", "pose = \"crawling\", speed_ratio = 0.2");
 replace("crawling = { image_tile = \"human-crawling\", height_ratio = 0.3 }",
  "crawling = { image_tile = \"human-crawling\", height_ratio = 0.3, width_ratio = 2 }");
 for (int mode = 0; mode < 5; ++mode) {
  core::World world("Climbing pose properties", 12, 3);
  auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, mode == 3 ? 3 : 1);
  if (mode != 3) world.addCorridor(mode >= 3 ? 2 : 1, 0, 12);
  uint32_t transit = bottom;
  if (mode < 2) transit = world.addStaircase(1, 0, 4, {3, CORE_SIDE_RIGHT, mode == 1 ? .03f : 0.f});
  else if (mode == 2) transit = world.addStairwell(1, 0, 2, 2, CORE_SIDE_LEFT);
  else if (mode == 3) { world.addSectorWalkway(bottom, 2, 4); world.addSectorLadder(bottom, 0, 4, {3, false, true}); }
  else transit = world.addLadder(1, 0, 4, {3, false, true}).ladder.sector->getIndex();
  std::string diagnostic; require(world.attachAgentType("transit-pose.agent.lua", definition.source, &diagnostic), diagnostic);
  world.finishBuild(); world.pauseSimulation();
  auto id = player(world, bottom, mode == 2 ? 3.f : mode >= 3 ? 4.35f : 3.85f, "TransitPose");
  auto actor = world.lookupAgent(id).entity;
  world.resumeSimulation();
  if (mode == 2) { world.requestPlayerDoorAction(); world.update(.3f); }
  world.setPlayerStairSelection(true, false); world.setPlayerMovementDirection(1); world.update(.6f);
  require(actor->getSector()->getIndex() == transit && actor->getGlobalPosition().y > 0.f,
   "Fixture must admit a real climbing flight: mode=" + std::to_string(mode));
  auto before = actor->getGlobalPosition(); world.setPlayerCrouch(true); world.update(.1f);
  require(actor->getPose() == Pose::Crawling, "Climbing must select and retain actual Crawling: mode=" + std::to_string(mode));
  auto expected = (mode >= 3 ? actor->getClimbSpeed() : mode == 1 ? actor->getWalkSpeed() : actor->getStationaryStairSpeed(true)) * .2f;
  if (mode == 1) expected += .03f;
  require(std::abs(actor->getGlobalPosition().distanceTo(before) - expected * .1f) < .0001f,
   "Climbing speed must use declared pose ratio without scaling belt speed: mode=" + std::to_string(mode));
  require(actor->getPoseImageTile() == "human-crawling" && std::abs(actor->getWidth() - .8f) < .0001f,
   "Climbing visual tile and width must follow Crawling");
  core::Vector2 min, max; actor->getBounds().getCurrentShape(min, max);
  require(std::abs(max.y - min.y - actor->getStandingHeight() * .3f) < .0001f,
   "Climbing bounds must use current pose height");
  if (mode == 2) {
   world.update(12.f); actor->getBounds().getCurrentShape(min, max);
   require(min.x >= 2.f + CORE_DOOR_X_INSET - .0001f && max.x <= 4.f - CORE_DOOR_X_INSET + .0001f,
    "Wide Crawling body must not penetrate Stairwell walls");
  }
  world.setPlayerMovementDirection(0); world.update(.1f);
  require(actor->getPose() == Pose::Crouching, "Stopping horizontal input must crouch on a supported climbing surface");
  world.setPlayerStairSelection(false, false); before = actor->getGlobalPosition(); world.update(.1f);
  require(std::abs(actor->getGlobalPosition().distanceTo(before) - (mode == 1 ? .003f : 0.f)) < .0001f,
   "Stationary crouching must retain climbing ownership; moving steps still carry");
  world.setPlayerCrouch(false); world.update(.1f);
  require(actor->getPose() == Pose::Standing, "C release must safely restore Standing while mounted");
 }
 for (int side : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
 for (bool descending : {false, true})
 for (float beltMagnitude : {.1f, .15f}) {
  core::World world("Pose-adjusted opposing belt entry", 12, 2);
  auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
  auto top = world.addCorridor(1, 0, 12);
  auto stairs = world.addStaircase(1, 0, 4, {3, side, descending ? beltMagnitude : -beltMagnitude});
  std::string diagnostic; require(world.attachAgentType("transit-pose.agent.lua", definition.source, &diagnostic), diagnostic);
  world.finishBuild(); world.pauseSimulation();
  int direction = (side == CORE_SIDE_RIGHT ? 1 : -1) * (descending ? -1 : 1);
  float endpoint = descending ? (side == CORE_SIDE_RIGHT ? 7.f : 4.f) : (side == CORE_SIDE_RIGHT ? 4.f : 7.f);
  auto id = player(world, descending ? top : bottom, endpoint - direction * .15f, "TransitPose");
  world.resumeSimulation(); world.setPlayerCrouch(true); world.setPlayerStairSelection(!descending, descending);
  world.setPlayerMovementDirection(direction); world.update(4.f);
  auto actor = world.lookupAgent(id).entity;
  require(actor->getPose() == Pose::Crawling && actor->getSector()->getIndex() != stairs && !actor->hasActiveLocomotionTask(),
   "Opposing Escalator entry must compare belt with actual Crawling speed, including equality");
  for (auto const& event : world.getSimulationEvents())
   require(event.type != core::SimulationEventType::TraversalRequestAdded, "Rejected Crawling entry must allocate no traversal");
 }
}
}
namespace agent_smoke {
void registerPlayerPoses(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlPoses", [](smoke::Context const& context) { controls(); capabilities(context); clearance(); climbing(); }});
}
}
