#include "Checks.h"
#include "core/World.h"
#include <fstream>
#include <sstream>
#include <cmath>

namespace {
using smoke::require;
using core::Pose;
void input(core::World& world, int direction, bool down, float seconds = .1f) {
 if (world.isTraversalTopologyDirty()) require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
 require(world.resumeSimulation(), "Cannot resume Player pose fixture");
 world.setPlayerMovementDirection(direction); world.setPlayerStairSelection(false, down); world.update(seconds);
}
core::AgentId player(core::World& world, uint32_t room, float x, std::string type = "Human") {
 auto id = world.createAgent(type, "Player", room, 0, x);
 require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign pose Player"); return id;
}
void controls() {
 core::World world("Player poses", 12, 2); auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
 world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 2.f);
 auto actor = [&] { return world.lookupAgent(id).entity; };
 input(world, 0, true); require(actor()->getPose() == Pose::Crouching && actor()->getGlobalPosition().x == 2.f, "Down must crouch without movement");
 input(world, 1, true); require(actor()->getPose() == Pose::Crawling && actor()->getGlobalPosition().x > 2.f, "Down+Right must crawl");
 auto x = actor()->getGlobalPosition().x; input(world, -1, true);
 require(actor()->getPose() == Pose::Crawling && actor()->getGlobalPosition().x < x, "Down+Left must crawl in reverse");
 input(world, 0, true); require(actor()->getPose() == Pose::Crouching, "Stopping horizontal input while Down remains held must crouch");
 input(world, 0, false); require(actor()->getPose() == Pose::Standing, "Releasing Down must stand when it fits");
 input(world, 0, true); world.pauseSimulation(); world.update(1.f);
 require(actor()->getPose() == Pose::Crouching, "Pause must freeze manual posture");
 world.resumeSimulation(); world.update(.1f); require(actor()->getPose() == Pose::Standing, "Resume must not revive cleared Down input");
 input(world, 0, true); world.pauseSimulation(); world.setAgentActive(id, false); world.resumeSimulation();
 world.setPlayerStairSelection(false, true); world.update(.1f); require(actor()->getPose() == Pose::Crouching, "Inactive Agent must not receive pose updates");
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
}
namespace agent_smoke {
void registerPlayerPoses(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlPoses", [](smoke::Context const& context) { controls(); capabilities(context); clearance(); }});
}
}
