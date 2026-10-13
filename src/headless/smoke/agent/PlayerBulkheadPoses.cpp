#include "Checks.h"
#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include <cmath>
#include <fstream>
#include <sstream>

namespace {
using smoke::require;
void attachCrawler(core::World& world, smoke::Context const& context, int widthRatio) {
 std::ifstream file(context.fixture("resources/test-worlds/human.agent.lua")); std::ostringstream buffer; buffer << file.rdbuf();
 require(bool(file), "Cannot read wide pose baseline"); std::string source = buffer.str();
 for (size_t at; (at = source.find("\"Human\"")) != std::string::npos;) source.replace(at, 7, "\"WideCrawler\"");
 auto at = source.find("image_tile = \"human-crawling\", height_ratio = 0.3"); require(at != std::string::npos, "Missing Crawling declaration");
 source.insert(at + std::string("image_tile = \"human-crawling\", height_ratio = 0.3").size(), ", width_ratio = " + std::to_string(widthRatio));
 std::string diagnostic; require(world.attachAgentType("wide-crawler.agent.lua", source, &diagnostic), diagnostic);
}
void standalone(bool reverse, bool closing, bool wide, float gap, smoke::Context const& context) {
 core::World world("Pose-aware live Bulkhead", 8, 2);
 auto left = world.addRoom("Left", 0, 0, 0, 4, 1); auto right = world.addRoom("Right", 0, 0, 4, 4, 1);
 core::World::CreateBulkheadDoorOptions options; options.activationMode = core::DoorActivationMode::Manual;
 options.controls[0] = options.controls[1] = false;
 auto created = world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
 (void)created;
 if (wide) attachCrawler(world, context, 2);
 world.finishBuild(); world.pauseSimulation();
 std::shared_ptr<core::BulkheadDoor> door;
 for (uint32_t i = 0; i < world.getSector(left)->getNumObjects(); ++i)
  if (auto object = std::dynamic_pointer_cast<core::BulkheadDoorSectorObject>(world.getSector(left)->getObject(i))) door = object->getDoor();
 require(bool(door), "Missing pose Bulkhead");
 auto source = reverse ? right : left; int direction = reverse ? -1 : 1;
 float start = reverse ? .35f : 3.65f;
 auto id = world.createAgent(wide ? "WideCrawler" : "Human", "Player", source, 0, start);
 require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Bulkhead pose Player");
 require(world.setDoorSpeedOverride(door->getTraversalResourceId(), .02f), "Cannot slow pose Bulkhead");
 world.resumeSimulation(); door->acquireOpenLease(); require(door->open(), "Cannot open pose Bulkhead");
 core::Vector2 leafMin, leafMax;
 for (int i = 0; i < 2000; ++i) {
  door->getCurrentShape(leafMin, leafMax); if (leafMin.y >= gap) break;
  world.update(core::World::getFixedTimestep());
 }
 require(leafMin.y >= gap && leafMin.y < gap + .01f, "Cannot stage physical leaf opening");
 auto actor = world.lookupAgent(id).entity;
 require(!door->admitsPlayerPassage(*actor, 0.f), "Partial leaf must block Standing");
 world.setPlayerStairSelection(false, true); world.update(core::World::getFixedTimestep());
 require(actor->getPose() == core::Pose::Crouching && door->admitsPlayerPassage(*actor, 0.f) == (gap > actor->getHeight()), "Geometry must use Crouching height, not automatically choose Crawling: pose=" + std::to_string((int)actor->getPose())
  + ", opening=" + std::to_string(door->getOpenPercentage()) + ", y=" + std::to_string(actor->getGlobalPosition().y));
 if (closing) { door->releaseOpenLease(); require(door->close(), "Cannot close pose Bulkhead"); }
 world.setPlayerMovementDirection(direction);
 for (int i = 0; i < 120 && direction * (4.f - actor->getGlobalPosition().x) > .002f; ++i) {
  world.update(core::World::getFixedTimestep()); core::Vector2 min, max; door->getCurrentShape(min, max);
  auto envelope = actor->getPoseEnvelope(actor->getPose());
  if (actor->getGlobalPosition().x + envelope->x * .5f > min.x && actor->getGlobalPosition().x - envelope->x * .5f < max.x)
   require(actor->getGlobalPosition().y + envelope->y <= min.y + .0001f, "Crawling body must never overlap live opening/closing leaf");
 }
 require(std::abs(actor->getGlobalPosition().x - 4.f) < .01f && actor->getPose() == core::Pose::Crawling, "Crawl must enter partial threshold in either direction");
 world.setPlayerMovementDirection(0); world.update(core::World::getFixedTimestep());
 auto stoppedPose = gap > actor->getPoseEnvelope(core::Pose::Crouching)->y ? core::Pose::Crouching : core::Pose::Crawling;
 require(actor->getPose() == stoppedPose, "Stopping may Crouch only when the larger envelope clears the leaf");
 world.setPlayerStairSelection(false, false); world.update(core::World::getFixedTimestep());
 require(actor->getPose() == stoppedPose, "Releasing Down must not expand to Standing through leaf");
 world.setPlayerMovementDirection(direction); world.update(1.f);
 require(actor->getSector()->getIndex() == (reverse ? left : right) && actor->getPose() == core::Pose::Standing,
   "Player must complete pose-aware passage then safely restore Standing");
}
void oversizedChambers(smoke::Context const& context) {
 for (int kind = 0; kind < 3; ++kind) {
  core::World world("Wide crawling chamber refusal", 12, 2);
  auto left = world.addRoom("Left", 0, 0, 0, 4, 1); world.addRoom("Right", 0, 0, 5, 4, 1);
  auto index = kind == 0 ? world.addAirlock(0, 0, 4, 1, 1.f)
    : world.addChamber(0, 0, 4, 1, true, kind == 1 ? core::ChamberSubtype::SecurityScanner : core::ChamberSubtype::Decontamination);
  (void)index; attachCrawler(world, context, 3); world.finishBuild(); world.pauseSimulation();
  auto id = world.createAgent("WideCrawler", "Player", left, 0, 3.65f);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign oversized chamber Player");
  world.resumeSimulation(); if (kind == 0) world.requestPlayerDoorAction(); world.update(.1f);
  world.setPlayerMovementDirection(-1); world.update(1.3f);
  world.setPlayerStairSelection(false, true); world.setPlayerMovementDirection(1); world.update(3.f);
  auto actor = world.lookupAgent(id).entity;
  require(actor->getPose() == core::Pose::Crawling && actor->getSector()->getIndex() == left,
    "Wider Crawling envelope must not enter a chamber whose interior cannot contain it");
  if (kind == 0) require(world.getSimulationSnapshot().airlocks.front().occupants.empty()
    && world.getSimulationSnapshot().airlocks.front().reservations.empty(), "Oversized crawler must not own Airlock capacity");
  else require(world.getSimulationSnapshot().securityScanners.front().occupants.empty()
    && world.getSimulationSnapshot().securityScanners.front().reservations.empty(), "Oversized crawler must not own Scanner/Decontamination capacity");
 }
}
}
namespace agent_smoke {
void registerPlayerBulkheadPoses(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlBulkheadPoses", [](smoke::Context const& context) {
  for (bool reverse : {false, true}) for (bool closing : {false, true}) for (bool wide : {false, true})
   for (float gap : {.18f, .30f}) standalone(reverse, closing, wide, gap, context);
  oversizedChambers(context);
 }});
}
}
