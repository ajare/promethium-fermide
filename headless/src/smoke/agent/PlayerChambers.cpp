#include "Checks.h"
#include "core/World.h"
#include "core/ChamberTransit.h"
#include "core/BulkheadDoor.h"
#include "core/Graph.h"
#include "core/MobilityProfile.h"
#include <cmath>

namespace {
using smoke::require;
struct Scene {
 core::World world{"Player Chambers", 20, 2};
 uint32_t left, right, index, source, target;
 core::AgentId id;
 std::shared_ptr<const core::ChamberTransit> chamber;
 int direction;
 bool crawledUnderStandingHeight{false};
 Scene(core::ChamberSubtype subtype, bool reverse, uint32_t width, bool wrongSide = false) : direction((reverse != wrongSide) ? -1 : 1) {
  left = world.addRoom("Left", 0, 0, 0, 4, 1);
  right = world.addRoom("Right", 0, 0, 4 + width, 4, 1);
  index = world.addChamber(0, 0, 4, width, !reverse, subtype);
  world.finishBuild(); world.pauseSimulation();
  require(world.setChamberConfiguration(index, .5f, .2f, .3f, .2f), "Cannot configure Player Chamber");
  source = (reverse != wrongSide) ? right : left; target = (reverse != wrongSide) ? left : right;
  id = world.createAgent("Player", source, 0, (reverse != wrongSide) ? .5f : 3.5f);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Chamber Player");
  require(world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None), "Cannot configure sensor-only Player");
  refresh(); require(world.resumeSimulation(), "Cannot resume Player Chamber");
 }
 void refresh() { chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world.getSector(index)); }
 core::Agent* actor() { return world.lookupAgent(id).entity; }
 auto const& snapshot() { return world.getSimulationSnapshot().securityScanners.front(); }
 void step() {
  world.update(core::World::getFixedTimestep());
  require(chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed(), "Chamber interlock must hold during manual movement");
  auto pos = actor()->getGlobalPosition(); auto envelope = actor()->getPoseEnvelope(actor()->getPose()).value();
  for (int side : {0, 1}) {
   core::Vector2 min, max; chamber->getDoor(side)->getCurrentShape(min, max);
   if (pos.x + envelope.x * .5f > min.x + .0001f && pos.x - envelope.x * .5f < max.x - .0001f) {
    crawledUnderStandingHeight = crawledUnderStandingHeight || (actor()->getPose() == core::Pose::Crawling && pos.y + actor()->getStandingHeight() > min.y);
    require(pos.y + envelope.y <= min.y + .0001f, "Player must not intersect Chamber Door bounding box");
   }
  }
 }
 void wait(int ticks) { for (int i = 0; i < ticks; ++i) step(); }
 void move(int sign) { world.setPlayerMovementDirection(sign); }
 void enter() {
  move(direction);
  for (int i = 0; i < 1800 && actor()->getSector()->getIndex() != index; ++i) step();
  move(0); require(actor()->getSector()->getIndex() == index, "Left/Right must enter sensor-operated Chamber");
  require(snapshot().occupants.size() == 1, "Manual entry must claim shared Chamber capacity");
 }
 void exit() {
  move(direction);
  for (int i = 0; i < 1800 && actor()->getSector()->getIndex() != target; ++i) step();
  move(0); require(actor()->getSector()->getIndex() == target, "Left/Right must exit processed Chamber");
  require(snapshot().occupants.empty(), "Manual exit must surrender Chamber capacity");
 }
};
void journey(core::ChamberSubtype subtype, bool reverse, uint32_t width) {
 Scene scene(subtype, reverse, width);
 auto original = scene.actor()->getGlobalPosition(); scene.wait(40);
 require(scene.actor()->getGlobalPosition().distanceTo(original) < .0001f, "Sensor must not approach or board Player");
 scene.enter(); auto stopped = scene.actor()->getGlobalPosition();
 scene.wait(40);
 require(scene.actor()->getGlobalPosition().distanceTo(stopped) < .0001f, "Release must not align Chamber occupant");
 scene.world.pauseSimulation();
 auto key = scene.world.addAccessPermission("Committed exit landing");
 require(scene.world.setLocationPermissionRequirement(scene.target, {key}), "Cannot protect admitted exit");
 scene.world.update(1.f); require(scene.actor()->getGlobalPosition().distanceTo(stopped) < .0001f, "Pause must freeze Chamber occupant");
 require(scene.world.resumeSimulation(), "Cannot resume admitted Chamber");
 if (subtype == core::ChamberSubtype::SecurityScanner && width > 1) {
  require(scene.chamber->getPhase() == core::SecurityScannerPhase::Positioning, "Scanner must wait for manual arrival at scan position");
  scene.move(scene.direction);
  for (int i = 0; i < 1200 && scene.chamber->getPhase() == core::SecurityScannerPhase::Positioning; ++i) scene.step();
  scene.move(0);
  require(scene.chamber->getPhase() != core::SecurityScannerPhase::Positioning, "Walking across scan centre must begin processing without snapping");
 }
 stopped = scene.actor()->getGlobalPosition();
 bool scanned = false;
 for (int i = 0; i < 2400 && !scene.chamber->getDoor(scene.chamber->getExitSide())->isOpen(); ++i) {
  scene.step(); scanned = scanned || scene.chamber->getPhase() == core::SecurityScannerPhase::Scanning;
 }
 require(scanned && scene.chamber->getDoor(scene.chamber->getExitSide())->isOpen(), "Chamber must complete specialized processing and open exit");
 scene.wait(300);
 require(scene.actor()->getSector()->getIndex() == scene.index && scene.actor()->getGlobalPosition().distanceTo(stopped) < .0001f,
   "Completed processing must never automatically eject Player");
 scene.move(-scene.direction); scene.wait(60); scene.move(0);
 require(scene.actor()->getSector()->getIndex() == scene.index, "Closed entrance must not bypass directional processing");
 scene.exit(); scene.world.resetSimulation(); scene.refresh();
 require(scene.actor()->getSector()->getIndex() == scene.source && scene.snapshot().occupants.empty()
   && scene.snapshot().reservations.empty() && scene.snapshot().crossings.empty(), "Reset must restore authored position and clear Chamber ownership");
}
void crawlJourney(core::ChamberSubtype subtype, bool reverse) {
 Scene scene(subtype, reverse, 1); scene.world.setPlayerCrouch(true);
 scene.move(scene.direction); bool checkedExpansion = false;
 for (int i = 0; i < 1800 && scene.actor()->getSector()->getIndex() != scene.index; ++i) {
  scene.step(); core::Vector2 min, max; scene.chamber->getDoor(scene.chamber->getEntrySide())->getCurrentShape(min, max);
  auto pos = scene.actor()->getGlobalPosition(); auto envelope = scene.actor()->getPoseEnvelope(scene.actor()->getPose());
  if (!checkedExpansion && !scene.snapshot().crossings.empty() && min.y < pos.y + scene.actor()->getStandingHeight() - .05f
    && pos.x + envelope->x * .5f > min.x && pos.x - envelope->x * .5f < max.x) {
   scene.move(0); scene.world.setPlayerCrouch(false); scene.step();
   require(scene.actor()->getPose() == core::Pose::Crawling, "C release mid-crossing must not stand into a Bulkhead leaf");
   scene.world.setPlayerCrouch(true); scene.move(scene.direction); checkedExpansion = true;
  }
 }
 scene.move(0); require(checkedExpansion && scene.actor()->getSector()->getIndex() == scene.index
   && scene.snapshot().occupants.size() == 1 && scene.actor()->getPose() == core::Pose::Crawling,
   "Chamber entry must retain explicit Crawling pose and safely reject mid-crossing expansion");
 scene.wait(1); require(scene.actor()->getPose() == core::Pose::Crouching, "Stopped admitted crawler must crouch when interior clearance permits");
 scene.exit();
 require(scene.actor()->getPose() == core::Pose::Crawling && scene.crawledUnderStandingHeight,
   "Scanner/Decontamination thresholds must admit explicit Crawling beneath a leaf that still blocks Standing");
 scene.world.setPlayerCrouch(false); scene.wait(1);
 require(scene.actor()->getPose() == core::Pose::Standing, "Completed crawler must safely stand on landing after releasing C");
}
void partialCrossing(core::ChamberSubtype subtype) {
 Scene scene(subtype, false, 2);
 scene.move(1);
 for (int i = 0; i < 1800 && scene.snapshot().crossings.empty(); ++i) scene.step();
 scene.move(0); require(!scene.snapshot().crossings.empty(), "Fixture must reach partially admitted Chamber crossing");
 auto stopped = scene.actor()->getGlobalPosition();
 scene.world.pauseSimulation(); require(scene.world.setAgentActive(scene.id, false), "Cannot suspend admitted Chamber Player");
 scene.world.resumeSimulation(); scene.wait(900);
 require(!scene.snapshot().crossings.empty() && scene.actor()->getGlobalPosition().distanceTo(stopped) < .0001f,
   "Inactive stopped doorway crossing must retain safe lease and capacity past timeout");
 scene.world.pauseSimulation(); scene.world.setAgentActive(scene.id, true); scene.world.resumeSimulation();
 scene.move(-1);
 for (int i = 0; i < 300 && !scene.snapshot().crossings.empty(); ++i) scene.step();
 scene.move(0);
 require(scene.snapshot().crossings.empty() && scene.snapshot().reservations.empty()
   && scene.actor()->getSector()->getIndex() == scene.source, "Retracing must cancel provisional Chamber admission safely");
}
void restrictions(core::ChamberSubtype subtype) {
 for (int scenario = 0; scenario < 3; ++scenario) {
  Scene scene(subtype, false, 2); scene.world.pauseSimulation();
  if (scenario == 0) {
   auto profile = scene.actor()->getEffectiveMobilityProfile().value; profile.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
   scene.world.setAgentIndividualMobilityProfile(scene.id, profile);
  } else if (scenario == 1) {
   auto key = scene.world.addAccessPermission("Exit"); scene.world.setLocationPermissionRequirement(scene.target, {key});
  } else require(scene.world.setRoomHeightScale(scene.target, .2f), "Cannot lower Chamber exit ceiling");
  scene.world.resumeSimulation(); scene.move(1); scene.wait(1600); scene.move(0);
  require(scene.actor()->getSector()->getIndex() == scene.source && scene.snapshot().occupants.empty(),
    "Chamber admission must preserve Mobility, permissions and Standing exit clearance");
 }
}
void walkingDuringProcessing(core::ChamberSubtype subtype) {
 Scene scene(subtype, false, 3); scene.enter();
 if (subtype == core::ChamberSubtype::SecurityScanner) {
  scene.move(1);
  for (int i = 0; i < 1200 && scene.chamber->getPhase() == core::SecurityScannerPhase::Positioning; ++i) scene.step();
  scene.move(0);
 }
 for (int i = 0; i < 2400 && scene.chamber->getPhase() != core::SecurityScannerPhase::Scanning; ++i) scene.step();
 require(scene.chamber->getPhase() == core::SecurityScannerPhase::Scanning, "Fixture must start specialized processing");
 scene.world.setPlayerCrouch(true); scene.wait(1);
 require(scene.actor()->getPose() == core::Pose::Crouching, "C must crouch inside the processing Chamber");
 auto original = scene.actor()->getGlobalPosition(); scene.move(1); scene.wait(4);
 require(scene.actor()->getPose() == core::Pose::Crawling, "C plus walking must crawl inside the Chamber");
 require(scene.actor()->getGlobalPosition().x > original.x && scene.snapshot().occupants.size() == 1,
   "Player must retain bounded manual walking and capacity during processing");
 scene.move(-1); scene.wait(4); scene.move(0);
 require(scene.actor()->getGlobalPosition().distanceTo(original) < .0001f, "Manual reversal during processing must preserve physical position");
 scene.wait(1); require(scene.actor()->getPose() == core::Pose::Crouching, "Stopped Chamber occupant must crouch while holding C");
 scene.world.setPlayerCrouch(false); scene.wait(1);
 require(scene.actor()->getPose() == core::Pose::Standing, "Chamber occupant must stand on C release when it fits");
}
void directionCapacityAndReset(core::ChamberSubtype subtype) {
 {
  Scene scene(subtype, false, 2, true); scene.move(scene.direction); scene.wait(1200);
  require(scene.actor()->getSector()->getIndex() == scene.source && scene.snapshot().occupants.empty()
    && scene.chamber->getPhase() == core::SecurityScannerPhase::Idle, "Wrong-side Player must not enter or call directional Chamber");
 }
 {
  Scene scene(subtype, false, 1);
  auto otherId = scene.world.createAgent("Autonomous", scene.left, 0, 3.f);
  auto other = scene.world.lookupAgent(otherId).entity;
  auto target = scene.world.getGraph()->getClosestVertexInSector(scene.world.getSector(scene.right).get(), {7.5f, 0.f});
  auto path = scene.world.getGraph()->calculatePath(other, target); require(bool(path), "Cannot route capacity witness"); other->setPath(path, true);
  for (int i = 0; i < 2400 && other->getSector()->getIndex() != scene.index; ++i) scene.step();
  require(other->getSector()->getIndex() == scene.index, "Capacity witness must occupy chamber first");
  scene.move(1);
  for (int i = 0; i < 2400 && other->getSector()->getIndex() == scene.index; ++i) {
   scene.step(); require(scene.actor()->getSector()->getIndex() == scene.source, "Player must not steal an occupied Chamber slot");
  }
  scene.move(0); require(other->getSector()->getIndex() == scene.right, "Blocked Player must not disrupt autonomous processing");
 }
 {
  Scene scene(subtype, false, 2); scene.enter(); scene.world.resetSimulation(); scene.refresh();
  require(scene.actor()->getSector()->getIndex() == scene.source && scene.snapshot().occupants.empty()
    && scene.snapshot().reservations.empty() && scene.snapshot().crossings.empty(), "Reset while aboard must clear manual Chamber ownership");
 }
}
void sharedBatch() {
 Scene scene(core::ChamberSubtype::Decontamination, false, 2); scene.enter();
 scene.move(1); for (int i = 0; i < 240 && scene.actor()->getGlobalPosition().x < 5.45f; ++i) scene.step(); scene.move(0);
 auto otherId = scene.world.createAgent("Autonomous", scene.left, 0, 3.5f);
 auto other = scene.world.lookupAgent(otherId).entity;
 auto target = scene.world.getGraph()->getClosestVertexInSector(scene.world.getSector(scene.right).get(), {7.5f, 0.f});
 auto path = scene.world.getGraph()->calculatePath(other, target); require(bool(path), "Cannot route shared Decontamination passenger"); other->setPath(path, true);
 bool shared = false;
 for (int i = 0; i < 4000 && other->getSector()->getIndex() != scene.right; ++i) {
  scene.step(); shared = shared || scene.snapshot().occupants.size() == 2;
 }
 require(shared && other->getSector()->getIndex() == scene.right && scene.actor()->getSector()->getIndex() == scene.index,
   "Autonomous Decontamination passenger must share batch and exit independently of Player");
 scene.exit();
}
}
namespace agent_smoke {
void registerPlayerChambers(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlChambers", [](smoke::Context const&) {
  for (auto subtype : {core::ChamberSubtype::SecurityScanner, core::ChamberSubtype::Decontamination}) {
   for (bool reverse : {false, true}) for (uint32_t width : {1u, 2u, 5u}) journey(subtype, reverse, width);
   partialCrossing(subtype); restrictions(subtype); walkingDuringProcessing(subtype); directionCapacityAndReset(subtype);
   crawlJourney(subtype, false); crawlJourney(subtype, true);
  }
  sharedBatch();
 }});
}
}
