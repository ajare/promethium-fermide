#include "Checks.h"
#include "core/World.h"
#include "core/AccessPanel.h"
#include "core/MobilityProfile.h"

namespace {
using smoke::require;
struct Scene {
 core::World world{"Player Access panels", 12, 2};
 uint32_t room, other;
 core::AgentId id;
 std::shared_ptr<const core::AccessPanel> panel;
 Scene(bool otherLayer = false, uint32_t actorLevel = 0, float x = 3.5f,
   uint32_t panelLevel = 0, core::AccessPanelGeometry geometry = {}) {
  room = world.addRoom("Host", 0, 0, 0, 8, 2); other = world.addRoom("Other Layer", 1, 0, 0, 8, 2);
  world.addSectorWalkway(room, 1, 3); world.addSectorWalkway(other, 1, 3);
  world.addAccessPanel(room, panelLevel, 3, geometry); world.finishBuild(); world.pauseSimulation();
  id = world.createAgent("Player", otherLayer ? other : room, actorLevel, x);
  require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Access panel Player"); refresh();
 }
 void refresh() {
  for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
   if (auto object = std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(world.getSector(room)->getObject(i))) panel = object->getPanel();
  require(bool(panel), "Missing Access panel fixture");
 }
 void press() {
  if (world.isTraversalTopologyDirty()) require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
  require(world.resumeSimulation(), "Cannot resume Access panel fixture");
  world.setPlayerStairSelection(true, false); world.requestPlayerDoorAction(); world.update(.1f);
 }
 std::vector<core::InteractionPointId> requests() {
  std::vector<core::InteractionPointId> result;
  for (auto const& event : world.getSimulationEvents())
   if (event.type == core::SimulationEventType::InteractionRequestAdded && event.interactionRequest.actor == id) result.push_back(event.interactionRequest.point);
  return result;
 }
};
void operation() {
 for (auto geometry : {core::AccessPanelGeometry{}, core::AccessPanelGeometry{0,0.25f,0.25f}}) {
  Scene scene(false, 0, 3.5f, 0, geometry); auto start = scene.world.lookupAgent(scene.id).entity->getGlobalPosition();
  scene.press(); require(scene.panel->getState() == core::AccessPanel::State::Opening && scene.panel->getProgress() > 0,
    "Fresh Up in front must begin normal Access panel opening animation: state=" + std::string(scene.panel->getStateName())
    + ", requests=" + std::to_string(scene.requests().size()) + ", progress=" + std::to_string(scene.panel->getProgress())
    + ", height=" + std::to_string(geometry.height));
  scene.press(); scene.world.update(3.f);
  require(scene.panel->getState() == core::AccessPanel::State::Open && scene.requests().size() == 1,
    "Held Up and busy accepted interactions must not queue another operation");
  scene.press(); scene.world.update(3.f); auto requests = scene.requests();
  require(scene.panel->getState() == core::AccessPanel::State::Closed && requests.size() == 2
    && requests[0] == scene.panel->getControl(core::AccessPanel::Action::Open)
    && requests[1] == scene.panel->getControl(core::AccessPanel::Action::Close), "Fresh Up must select state-appropriate owned open/close controls");
  require(scene.world.lookupAgent(scene.id).entity->getGlobalPosition().distanceTo(start) < .0001f, "Panel action must not approach or align Player");
  scene.press(); scene.world.pauseSimulation(); auto progress = scene.panel->getProgress(); scene.world.update(3.f);
  require(scene.panel->getProgress() == progress, "Pause must freeze Access panel animation");
  scene.world.setAgentIndividualObjectUsage(scene.id, core::ObjectUsage::None); scene.world.resumeSimulation(); scene.world.update(3.f);
  require(scene.panel->getState() == core::AccessPanel::State::Open, "Accepted panel action must complete despite later capability loss");
  scene.world.resetSimulation(); scene.refresh(); require(scene.panel->getState() == core::AccessPanel::State::Closed, "Reset must restore closed panel state");
 }
 Scene zero(false, 0, 3.5f, 0, {0,0,0}); zero.press();
 require(zero.panel->getState() == core::AccessPanel::State::Open, "Zero-height panels retain normal instantaneous opening");
 zero.press(); require(zero.panel->getState() == core::AccessPanel::State::Closed, "Zero-height panels must also close with fresh Up");
 Scene upstairs(false, 1, 3.5f, 1); upstairs.press(); require(upstairs.panel->getState() == core::AccessPanel::State::Opening,
   "Access panel operation must work on supported upper Levels");
}
void gates() {
 for (int scenario = 0; scenario < 7; ++scenario) {
  Scene scene(scenario == 0, scenario == 1 ? 1 : 0, scenario == 2 ? 3.75f : 3.5f);
  if (scenario == 2) scene.world.setAgentIndividualObjectUsageDistance(scene.id, .125f);
  if (scenario == 3 || scenario == 4) scene.world.setAgentIndividualObjectUsage(scene.id,
    scenario == 3 ? core::ObjectUsage::None : core::ObjectUsage::RemoteControl);
  if (scenario == 4) scene.world.setAgentIndividualRemoteAccessPanels(scene.id, true);
  if (scenario == 5) {
   auto profile = scene.world.lookupAgent(scene.id).entity->getEffectiveMobilityProfile().value;
   profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse); scene.world.setAgentIndividualMobilityProfile(scene.id, profile);
  }
  if (scenario == 6) scene.world.setAgentActive(scene.id, false);
  auto start = scene.world.lookupAgent(scene.id).entity->getGlobalPosition(); scene.press(); scene.world.update(3.f);
  require(scene.requests().empty() && scene.panel->getState() == core::AccessPanel::State::Closed
    && scene.world.lookupAgent(scene.id).entity->getGlobalPosition().distanceTo(start) < .0001f,
    "Rejected Up must preserve Location/Layer, Level, arm reach, Arms, Buttons Mobility and activation gates without approach");
 }
 Scene exact(false, 0, 3.625f); exact.world.setAgentIndividualObjectUsageDistance(exact.id, .125f); exact.press();
 require(exact.panel->getState() == core::AccessPanel::State::Opening, "Inclusive physical arm reach must operate panel");
}
void priorityAndCancellation() {
 {
  Scene scene; scene.world.addSectorLadder(scene.room, 0, 3, {2, false, true});
  require(scene.world.rebuildTraversalTopology(), scene.world.getTopologyDiagnostic()); scene.world.resumeSimulation();
  scene.world.setPlayerMovementDirection(1); scene.press();
  require(scene.requests().empty() && scene.panel->getState() == core::AccessPanel::State::Closed
    && scene.world.lookupAgent(scene.id).entity->getGlobalPosition().y > 0, "Contextual climbing must retain Up priority over Access panels");
 }
 {
  Scene scene(false, 0, 3.75f); scene.world.setAgentIndividualObjectUsageDistance(scene.id, .25f);
  auto other = scene.world.createAgent("Other operator", scene.room, 0, 3.5f);
  require(scene.world.resumeSimulation() && bool(scene.world.requestAccessPanel(scene.panel->getId(), core::AccessPanel::Action::Open, other)),
    "Cannot queue other panel operator");
  scene.world.setPlayerMovementDirection(1); scene.press();
  bool cancelled = false;
  for (auto const& event : scene.world.getSimulationEvents())
   if (event.type == core::SimulationEventType::InteractionRequestChanged && event.interactionRequest.actor == scene.id
     && event.interactionRequest.result == core::InteractionResult::Cancelled) cancelled = true;
  require(cancelled && scene.panel->getState() == core::AccessPanel::State::Opening,
    "Leaving physical reach must cancel queued Player panel operation without undoing another operator's accepted action");
 }
}
}
namespace agent_smoke {
void registerPlayerAccessPanels(std::vector<smoke::Check>& checks) {
 checks.push_back({"playerControlAccessPanels", [](smoke::Context const&) { operation(); gates(); priorityAndCancellation(); }});
}
}
