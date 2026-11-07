#pragma once

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <core/World.h>
#include <core/LiftTransit.h>
#include <core/LiftSectorObject.h>

namespace pf::application {

// Presentation-only observer: never drains events, submits commands or changes World.
class PlayDebug {
  bool mEnabled{false};
  core::TraversalResourceId mLift{};
  uint64_t mSequence{0}, mTick{0};
public:
  bool enabled() const { return mEnabled; }
  core::TraversalResourceId lift() const { return mLift; }
  void toggle(core::World const& world) {
    mEnabled = !mEnabled;
    mLift = {};
    auto const& events = world.getSimulationEvents();
    mSequence = events.empty() ? 0 : events.back().sequence;
    mTick = world.getSimulationSnapshotView().tick;
  }
  void observe(core::World const& world) {
    if (!mEnabled) return;
    auto const& snapshot = world.getSimulationSnapshotView();
    if (snapshot.tick < mTick) { mLift = {}; mSequence = 0; }
    mTick = snapshot.tick;
    auto const& events = world.getSimulationEvents();
    auto first = std::upper_bound(events.begin(), events.end(), mSequence,
      [](uint64_t sequence, core::SimulationEvent const& event) { return sequence < event.sequence; });
    auto player = world.getPlayerControlledAgent();
    for (auto it = first; it != events.end(); ++it) {
      auto const& operation = it->deviceOperation;
      if (it->type != core::SimulationEventType::DeviceOperationChanged
          || operation.state != core::DeviceOperationState::Succeeded || !player
          || (operation.requester != player && std::find(operation.requesters.begin(), operation.requesters.end(), player) == operation.requesters.end())
          || (operation.command.type != core::DeviceCommandType::CallLift
              && operation.command.type != core::DeviceCommandType::SelectLiftDestination)) continue;
      auto resource = world.lookupTraversalResource(operation.command.traversalResource);
      if (resource && resource.entity->isLift()) mLift = operation.command.traversalResource;
    }
    if (!events.empty()) mSequence = events.back().sequence;
    if (mLift && !world.lookupTraversalResource(mLift)) mLift = {};
  }
};

struct LiftDebugReadout {
  core::Vector2 anchor;
  uint32_t layer{0};
  std::vector<std::string> lines;
};

inline std::optional<LiftDebugReadout> liftDebugReadout(core::World const& world, core::TraversalResourceId id) {
  auto const& snapshot = world.getSimulationSnapshotView();
  auto found = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
    [&](auto const& resource) { return resource.id == id && resource.isLift; });
  if (found == snapshot.traversalResources.end() || !found->liftSector) return {};
  auto const& lift = *found;
  auto sector = world.getSector((uint32_t)lift.liftSector.value - 1);
  if (!sector) return {};
  std::shared_ptr<const core::Lift> shape;
  if (auto transit = std::dynamic_pointer_cast<const core::LiftTransit>(sector)) shape = transit->getLift();
  else for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
    if (auto object = std::dynamic_pointer_cast<const core::LiftSectorObject>(sector->getObject(i)))
      if (object->getLift()->getTraversalResourceId() == id) { shape = object->getLift(); break; }
  if (!shape) return {};
  auto number = [](double value) {
    std::ostringstream text; text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return text.str();
  };
  auto seconds = [&](uint64_t ticks) { return number(ticks * double(core::World::getFixedTimestep())) + " s"; };
  auto stop = [](uint32_t index) { return index == ~0u ? std::string("none") : std::to_string(index); };
  static constexpr char const* phases[] = {"Idle", "Moving", "Opening", "Disembarking", "Boarding", "Closing"};
  bool dwellActive = lift.liftStopPhase == core::LiftStopPhase::Boarding || lift.liftStopPhase == core::LiftStopPhase::Disembarking;
  uint64_t elapsed = snapshot.tick >= lift.liftServiceStartedTick ? snapshot.tick - lift.liftServiceStartedTick : 0;
  uint64_t dwellLeft = elapsed < lift.liftMinimumDwellTicks ? lift.liftMinimumDwellTicks - elapsed : 0;
  uint64_t cutoffLeft = snapshot.tick < lift.liftBoardingCutoffTick ? lift.liftBoardingCutoffTick - snapshot.tick : 0;
  LiftDebugReadout result;
  result.anchor = {shape->getPosition().x + shape->getSize().x + .15f, lift.liftPosition + .9f};
  result.layer = sector->getLayerIndex();
  result.lines = {
    "Lift " + std::to_string(id.value) + " [F11 debug]",
    "Level: " + number(lift.liftPosition),
    std::string("Phase: ") + phases[(int)lift.liftStopPhase] + (lift.liftBroken ? "  BROKEN" : ""),
    "Stop: " + stop(lift.liftCurrentStop) + " -> " + stop(lift.liftTargetStop),
    "Dwell elapsed: " + seconds(elapsed) + (dwellActive ? "" : " (inactive)"),
    "Dwell min/left: " + seconds(lift.liftMinimumDwellTicks) + " / " + seconds(dwellLeft),
    "Boarding max: " + seconds(lift.liftMaximumBoardingTicks),
    "Cutoff left: " + (lift.liftBoardingCutoffTick ? seconds(cutoffLeft) : std::string("not started")),
    "Tick/start/cutoff: " + std::to_string(snapshot.tick) + "/" + std::to_string(lift.liftServiceStartedTick) + "/" + std::to_string(lift.liftBoardingCutoffTick),
    "Occupants/reserved: " + std::to_string(lift.occupantCount) + "/" + std::to_string(lift.admissionReservationCount),
    std::string("Car door: ") + (lift.liftCarDoorOpen ? "open" : "closed") + "  Admission: " + (lift.liftAcceptingBoarders ? "yes" : "no")
  };
  return result;
}
} // namespace pf::application
