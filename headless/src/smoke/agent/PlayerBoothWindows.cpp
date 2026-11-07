#include "Checks.h"
#include "core/World.h"
#include "core/WindowSectorObject.h"
#include "core/MobilityProfile.h"
#include "core/Dumbwaiter.h"
#include <cmath>

namespace {
using smoke::require;
struct Scene {
 core::World world{"Player BoothWindow", 12, 2};
 uint32_t front, back;
 core::AgentId id;
 std::shared_ptr<const core::BoothWindow> booth;
 Scene(bool frontSide = false, uint32_t level = 0, float x = 3.5f) {
  front = world.addRoom("Front", 0, 0, 0, 8, 2);
  back = world.addRoom("Back", 1, 0, 0, 8, 2);
  world.addSectorWalkway(front, 1, 3); world.addSectorWalkway(back, 1, 3);
  world.addBoothWindow(0, 0, 3, core::Window::State::Closed);
  world.finishBuild(); world.pauseSimulation();
  id = world.createAgent("Player", frontSide ? front : back, level, x);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign BoothWindow Player");
  refresh();
 }
 void refresh() {
  for (uint32_t i = 0; i < world.getSector(front)->getNumObjects(); ++i)
   if (auto object = std::dynamic_pointer_cast<const core::WindowSectorObject>(world.getSector(front)->getObject(i)))
    if (auto found = std::dynamic_pointer_cast<const core::BoothWindow>(object->getWindow())) booth = found;
  require(bool(booth), "Missing BoothWindow fixture");
 }
 void press(float seconds = .1f) {
  if (world.isTraversalTopologyDirty()) require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
  require(world.resumeSimulation(), "Cannot resume BoothWindow Player");
  world.setPlayerStairSelection(true, false); world.requestPlayerDoorAction(); world.update(seconds);
 }
 size_t requests() {
  size_t count = 0;
  for (auto const& event : world.getSimulationEvents())
   if (event.type == core::SimulationEventType::InteractionRequestAdded && event.interactionRequest.actor == id
     && event.interactionRequest.point == booth->getPanel()) ++count;
  return count;
 }
};
void operation() {
 Scene scene; auto start = scene.world.lookupAgent(scene.id).entity->getGlobalPosition();
 scene.press(); require(scene.booth->getTargetOpen() && scene.booth->getProgress() > 0 && scene.booth->getProgress() < 1,
   "Fresh Up must animate opening via owned back-side control: requests=" + std::to_string(scene.requests())
   + ", progress=" + std::to_string(scene.booth->getProgress()) + ", target=" + std::to_string(scene.booth->getTargetOpen()));
 scene.world.update(1.f);
 require(scene.booth->getProgress() == 1.f && scene.requests() == 1, "Held Up must not repeat shutter toggle");
 require(scene.world.lookupAgent(scene.id).entity->getSector()->getIndex() == scene.back
   && scene.world.lookupAgent(scene.id).entity->getGlobalPosition().distanceTo(start) < .0001f, "Shutter control must neither approach nor cross Layers");
 scene.press(); scene.world.update(1.f);
 require(!scene.booth->getTargetOpen() && scene.booth->getProgress() == 0.f && scene.requests() == 2, "Second fresh Up must close BoothWindow");
 scene.press(); scene.press(); scene.world.update(1.f);
 require(scene.booth->getTargetOpen() && scene.booth->getProgress() == 1.f && scene.requests() == 3,
   "Busy accepted interaction must retain normal deduplication rather than queue another toggle");
 scene.press(); scene.world.update(1.f);
 require(!scene.booth->getTargetOpen() && scene.booth->getProgress() == 0.f, "Completed operation must permit another fresh close press");
 scene.press(); scene.world.pauseSimulation(); auto progress = scene.booth->getProgress(); scene.world.update(1.f);
 require(scene.booth->getProgress() == progress, "Pause must freeze Player shutter animation");
 scene.world.resumeSimulation(); scene.world.update(1.f); require(scene.booth->getProgress() == 1.f, "Accepted toggle must finish on resume");
 scene.world.resetSimulation(); scene.refresh();
 require(!scene.booth->getTargetOpen() && scene.booth->getProgress() == 0.f, "Reset must restore authored shutter state");
}
void gates() {
 for (int scenario = 0; scenario < 8; ++scenario) {
  Scene scene(scenario == 0, scenario == 1 ? 1 : 0, scenario == 2 ? 3.75f : 3.5f);
  if (scenario == 2) require(scene.world.setAgentIndividualObjectUsageDistance(scene.id, .125f), "Cannot shorten arm reach");
  if (scenario == 3 || scenario == 4) scene.world.setAgentIndividualObjectUsage(scene.id,
    scenario == 3 ? core::ObjectUsage::None : core::ObjectUsage::RemoteControl);
  if (scenario == 4) scene.world.setAgentIndividualRemoteBoothWindowShutters(scene.id, true);
  if (scenario == 5) {
   auto profile = scene.world.lookupAgent(scene.id).entity->getEffectiveMobilityProfile().value;
   profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse); scene.world.setAgentIndividualMobilityProfile(scene.id, profile);
  }
  if (scenario == 6) {
   auto key = scene.world.addAccessPermission("Shutter control");
   require(scene.world.setInteractionPointPermissionRequirement(scene.booth->getPanel(), {key}), "Cannot protect back-side panel");
   scene.world.setAgentIndividualPermissionAdherence(scene.id, false);
  }
  if (scenario == 7) scene.world.setAgentActive(scene.id, false);
  auto position = scene.world.lookupAgent(scene.id).entity->getGlobalPosition(); scene.press(); scene.world.update(1.f);
  require(scene.requests() == 0 && !scene.booth->getTargetOpen() && scene.booth->getProgress() == 0.f,
    "Player shutter operation must respect back-side ownership, Level, reach, Arms, Mobility, permissions and activation");
  require(scene.world.lookupAgent(scene.id).entity->getGlobalPosition().distanceTo(position) < .0001f, "Rejected control must not approach shutter");
 }
 {
  Scene scene(false, 0, 3.625f);
  require(scene.world.setAgentIndividualObjectUsageDistance(scene.id, .125f), "Cannot configure exact arm reach");
  scene.press(); require(scene.booth->getTargetOpen(), "Inclusive arm-length boundary must admit back-side operation");
 }
 {
  Scene scene; auto key = scene.world.addAccessPermission("Authorized back-side operator");
  require(scene.world.setInteractionPointPermissionRequirement(scene.booth->getPanel(), {key})
    && scene.world.grantAgentAccessPermission(scene.id, key), "Cannot authorize panel operator");
  scene.press(); scene.world.pauseSimulation(); scene.world.setAgentRuntimeAccessPermissionGrant(scene.id, key, false);
  scene.world.resumeSimulation(); scene.world.update(1.f);
  require(scene.booth->getProgress() == 1.f, "Authorization loss must not undo an accepted toggle");
 }
}
void priorityAndOwnedApertures() {
 {
  Scene scene; scene.world.addSectorLadder(scene.back, 0, 3, {2, false, true});
  require(scene.world.rebuildTraversalTopology(), scene.world.getTopologyDiagnostic()); scene.refresh();
  require(scene.world.resumeSimulation(), "Cannot resume priority fixture");
  scene.world.setPlayerMovementDirection(1); scene.press();
  require(scene.requests() == 0 && !scene.booth->getTargetOpen()
    && scene.world.lookupAgent(scene.id).entity->getGlobalPosition().y > 0,
    "Contextual Ladder mounting must retain priority over BoothWindow shutter control");
 }
 {
  core::World world("Owned aperture remains interlocked", 12, 2);
  auto front = world.addRoom("Landing", 0, 0, 0, 8, 2); world.addSectorWalkway(front, 1, 3);
  auto unitId = world.addDumbwaiter(1, 0, 3); world.finishBuild(); world.pauseSimulation();
  auto id = world.createAgent("Player", front, 0, 3.5f); world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {});
  auto unit = world.lookupDumbwaiter(unitId); auto aperture = unit->getAperture(0); auto original = aperture->getTargetOpen();
  world.resumeSimulation(); world.requestPlayerDoorAction(); world.update(.1f);
  require(!aperture->getPanel() && aperture->getTargetOpen() == original && !unit->isBusy(),
    "Player must not invent shutter controls or bypass Dumbwaiter ownership/interlocks");
 }
}
void cancellation() {
 Scene scene(false, 0, 3.75f);
 require(scene.world.setAgentIndividualObjectUsageDistance(scene.id, .25f), "Cannot configure full panel reach");
 auto other = scene.world.createAgent("Other operator", scene.back, 0, 3.5f);
 require(scene.world.resumeSimulation() && bool(scene.world.requestInteraction(scene.booth->getPanel(), other)), "Cannot occupy back-side panel");
 scene.world.setPlayerMovementDirection(1); scene.press();
 bool cancelled = false;
 for (auto const& event : scene.world.getSimulationEvents())
  if (event.type == core::SimulationEventType::InteractionRequestChanged && event.interactionRequest.actor == scene.id
    && event.interactionRequest.result == core::InteractionResult::Cancelled) cancelled = true;
 require(cancelled && scene.booth->getTargetOpen(), "Leaving physical reach must cancel queued Player toggle, not reverse other operator's accepted toggle");
}
}
namespace agent_smoke {
void registerPlayerBoothWindows(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlBoothWindows", [](smoke::Context const&) { operation(); gates(); priorityAndOwnedApertures(); cancellation(); }});
}
}
