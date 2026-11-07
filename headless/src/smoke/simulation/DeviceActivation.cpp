// Device activation and supersession boundaries, for ticket #623.
//
// Activation used to be spread inline across advanceDeviceOperations(),
// moveInteractions() and the Dumbwaiter-specific handling. It now lives in
// one helper per device family, but every helper is still invoked from the
// exact phase and dispatch position the inline code occupied. These checks
// pin that contract so a later consolidation cannot silently shift a
// device's activation by a tick:
//
//   * submitting an intent is not activating it: a BoothWindow shutter
//     starts moving only on the ResourceAdvancement tick its operation
//     activates on
//   * a competing target supersedes the operation which was travelling the
//     other way on the same device on that same activation tick, while the
//     device keeps its physical position (no teleport, no snap-back)
//   * an Access panel press activates at the end of the pressing Agent's
//     Movement-phase press interval, and a reversal keeps the leaf's
//     physical position
//   * a Dumbwaiter reserves busy ownership at the admission boundary, and
//     competing presses keep their existing refusal result
//   * an implicit manual Door is revalidated at activation: an operation
//     whose requester cannot still operate it from where it stands is
//     Rejected on that tick, and one which can is not
//
// Registration: simulation smoke module.

#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/SimulationCoordinator.h"
#include "core/Dumbwaiter.h"
#include "core/Window.h"
#include "core/AccessPanel.h"
#include "core/ExtensibleObject.h"
#include "../support/BrokenExtensibleFixture.h"
#include "../support/DumbwaiterFixture.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
	using smoke::require;
	using core::DeviceCommand;
	using core::DeviceCommandType;
	using core::DeviceOperationState;

	DeviceOperationState stateOf(core::World& world, core::DeviceOperationId id)
	{
		auto operation = world.lookupDeviceOperation(id);
		return operation ? operation.entity->getState() : DeviceOperationState::Rejected;
	}

	// A BoothWindow with one shutter and no Agent: the editor submission
	// seam creates the same operations an Interaction binding does.
	struct ShutterFixture
	{
		core::World world{"Activation supersession", 8, 3};
		std::shared_ptr<const core::BoothWindow> booth;

		ShutterFixture()
		{
			world.addRoom("Front", 0, 0, 0, 7, 2);
			world.addRoom("Back", 1, 0, 0, 7, 2);
			booth = std::static_pointer_cast<const core::BoothWindow>(
				world.addBoothWindow(0, 0, 3, core::Window::State::Closed).object);
			world.finishBuild();
			world.markSaved();
		}

		DeviceCommand set(bool desired) const
		{
			DeviceCommand command;
			command.type = DeviceCommandType::SetBoothWindowState;
			command.desiredState = desired;
			command.boothWindow = booth->getDeviceId();
			return command;
		}
		float progress() const { return booth->getProgress(); }
	};

	// Submission is not activation: the shutter only starts moving on the
	// tick the operation activates, in ResourceAdvancement.
	void boothWindowActivatesOnItsOwnTick(smoke::Context const&)
	{
		ShutterFixture f;
		auto opening = f.world.submitDeviceCommand(f.set(true));
		require(bool(opening), "Editor shutter submission refused");
		require(stateOf(f.world, opening) == DeviceOperationState::Pending
			&& f.progress() == 0.0f && !f.booth->getTargetOpen(),
			"Submission activated the shutter or its operation");
		f.world.advanceTick();
		require(stateOf(f.world, opening) == DeviceOperationState::Running
			&& f.booth->getTargetOpen() && std::abs(f.progress() - 1.0f / 48) < 1e-5f,
			"Shutter activation did not happen on the first tick after submission");
	}

	// A competing target retires the operation which was travelling the
	// other way on the same tick it activates, and the shutter reverses from
	// the physical position it had already reached.
	void boothWindowCompetingTargetSupersedesSameTick(smoke::Context const&)
	{
		ShutterFixture f;
		auto opening = f.world.submitDeviceCommand(f.set(true));
		f.world.advanceTicks(12);
		require(stateOf(f.world, opening) == DeviceOperationState::Running
			&& std::abs(f.progress() - 0.25f) < 1e-5f, "Opening travel did not reach the reversal point");

		auto reversing = f.world.submitDeviceCommand(f.set(false));
		require(reversing != opening, "Opposite desired-state command coalesced with the running one");
		require(stateOf(f.world, reversing) == DeviceOperationState::Pending
			&& stateOf(f.world, opening) == DeviceOperationState::Running
			&& std::abs(f.progress() - 0.25f) < 1e-5f,
			"Competing submission superseded travel or moved the shutter before activation");

		f.world.advanceTick();
		require(stateOf(f.world, reversing) == DeviceOperationState::Running
			&& stateOf(f.world, opening) == DeviceOperationState::Cancelled,
			"Competing target did not supersede the running travel on its activation tick");
		// The shutter travel loop runs after the operation loop in the same
		// ResourceAdvancement tick, so the reversal starts from 0.25 and
		// takes exactly one step: the physical position is retained, not
		// restarted from either end.
		require(std::abs(f.progress() - (0.25f - 1.0f / 48)) < 1e-5f && !f.booth->getTargetOpen(),
			"Supersession moved the shutter instead of reversing it from its physical position");
		// The reversal keeps the physical position and closes proportionally.
		f.world.advanceTicks(12);
		require(f.progress() == 0.0f && !f.booth->getTargetOpen(),
			"Reversal did not continue from the retained physical position");
	}

	// Two extensible intents with opposite targets: the later one to
	// activate supersedes the earlier Running operation on that same tick,
	// and the physical extension is not rewound by the supersession.
	void extensibleOppositeTargetSupersedesSameTick(smoke::Context const&)
	{
		broken_extensible::Scene scene(broken_extensible::Kind::RoomLadder, false);
		auto& world = *scene.world;
		auto const resource = scene.resource;
		scene.device->extend();
		scene.device->update(scene.device->getExtendRetractTime() * 0.5f);
		auto const mid = scene.device->getExtendedPercentage();
		require(std::abs(mid - 0.5f) < 1e-3f, "Fixture did not reach a mid-extension position");

		auto presser = world.createAgent("Second presser", scene.room, 0, 0.5f);
		require(world.resumeSimulation(), "Extensible fixture resume failed");

		core::InteractionBinding extendBinding;
		extendBinding.command = { DeviceCommandType::SetExtendedState, {}, true, resource };
		auto extendPoint = world.createInteractionPoint("Extend control", core::SectorId{ scene.room + 1u },
			scene.agent->getGlobalPosition(), 1.0f, 0.0f, { extendBinding });
		core::InteractionBinding retractBinding;
		retractBinding.command = { DeviceCommandType::SetExtendedState, {}, false, resource };
		auto retractPoint = world.createInteractionPoint("Retract control", core::SectorId{ scene.room + 1u },
			world.lookupAgent(presser).entity->getGlobalPosition(), 1.0f, 0.0f, { retractBinding });
		require(bool(extendPoint) && bool(retractPoint), "Extensible control bindings refused");

		core::SimulationCoordinator coordinator(world);
		auto extendRequest = coordinator.requestInteractionWhilePassing(extendPoint, scene.id);
		auto retractRequest = coordinator.requestInteractionWhilePassing(retractPoint, presser);
		require(bool(extendRequest) && bool(retractRequest), "Competing extensible passing press refused");
		auto extendOperation = world.lookupInteractionRequest(extendRequest).entity->getOperations().front().first;
		auto retractOperation = world.lookupInteractionRequest(retractRequest).entity->getOperations().front().first;
		require(stateOf(world, extendOperation) == DeviceOperationState::Pending
			&& stateOf(world, retractOperation) == DeviceOperationState::Pending,
			"Extensible press activated its operation before the activation phase");

		world.advanceTick();
		require(stateOf(world, retractOperation) == DeviceOperationState::Running
			&& stateOf(world, extendOperation) == DeviceOperationState::Cancelled,
			"Opposite extensible target did not supersede the running operation on its activation tick");
		auto const after = scene.device->getExtendedPercentage();
		require(after > 0.0f && after < 1.0f && std::abs(after - mid) < 0.05f,
			"Extensible supersession rewound the physical extension instead of reversing from it");
		require(world.getSimulationSnapshot().traversalResources.front().retractionPending,
			"Superseding retract intent did not withdraw admission on its activation tick");
	}

	// The panel press activates at the end of the pressing Agent's press
	// interval, and a reversal keeps the leaf's physical position.
	void accessPanelReversalKeepsLeafPosition(smoke::Context const&)
	{
		core::World world{"Panel activation", 9, 3};
		auto room = world.addRoom("Room", 0, 0, 0, 8, 2);
		auto made = world.addAccessPanel(room, 0, 4);
		world.finishBuild();
		auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(
			made.sector->getObject(made.index))->getPanel();
		auto opener = world.createAgent("Opener", room, 0, 4.5f);
		auto closer = world.createAgent("Closer", room, 0, 4.5f);

		auto open = world.requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, opener);
		require(bool(open), "Panel Open refused");
		world.advanceTick();
		require(panel->getState() == core::AccessPanel::State::Opening, "Open did not activate on its tick");
		world.advanceTicks(15);
		require(std::abs(panel->getProgress() - 0.5f) < 1e-5f, "Panel did not reach the mid position");
		auto const mid = panel->getProgress();
		// One fixed tick of leaf travel at the configured speed: the most the
		// reversal tick may move the leaf before the reversal takes hold.
		auto const step = panel->getSpeed() * core::World::getFixedTimestep() / panel->getGeometry().height;

		auto close = world.requestAccessPanel(panel->getId(), core::AccessPanel::Action::Close, closer);
		require(bool(close), "Panel Close refused while opening");
		require(panel->getProgress() == mid && panel->getState() == core::AccessPanel::State::Opening,
			"Competing panel request changed the leaf target before its own activation");
		world.advanceTick();
		require(panel->getState() == core::AccessPanel::State::Closing,
			"Panel reversal did not activate on the closer's own tick");
		require(panel->getProgress() >= mid && panel->getProgress() <= mid + step + 1e-5f,
			"Panel reversal teleported the leaf instead of reversing from its physical position");
		require(world.lookupInteractionRequest(open).entity->getResult() == core::InteractionResult::Failed,
			"Superseded panel travel did not fail its required interaction");
		world.advanceTicks(60);
		require(panel->getState() == core::AccessPanel::State::Closed, "Reversal did not finish closing");
	}

	// A Dumbwaiter reserves its busy ownership at the admission boundary:
	// the accepted press owns the cycle immediately, and every competing
	// press keeps its existing refusal result.
	void dumbwaiterBusyOwnershipIsReservedAtAdmission(smoke::Context const&)
	{
		auto world = dumbwaiter_fixture::make();
		auto id = world->addDumbwaiter(1, 0, 2, { 0, 2.0f });
		world->finishBuild();
		require(world->resumeSimulation(), "Dumbwaiter fixture resume failed");
		auto unit = world->lookupDumbwaiter(id);
		require(bool(unit), "Dumbwaiter fixture missing");

		auto accepted = world->pressDumbwaiterLanding(id, 1);
		require(accepted && stateOf(*world, accepted) == DeviceOperationState::Running
			&& unit->isBusy() && unit->getOperation() == accepted,
			"Press did not reserve the cycle at the admission boundary");
		for (uint32_t stop : { 0u, 1u })
		{
			auto competing = world->pressDumbwaiterLanding(id, stop);
			require(competing && competing != accepted
				&& stateOf(*world, competing) == DeviceOperationState::Rejected,
				"Competing press was buffered, coalesced or accepted");
			require(unit->getOperation() == accepted && unit->isBusy(),
				"Competing press disturbed the owning cycle");
		}
		// The reservation survives the whole cycle - close, travel, open -
		// and is released exactly when the car has fully opened at its
		// destination, which is also where the operation succeeds.
		world->advanceTicks(48);
		require(unit->isBusy() && unit->getPhase() == core::DumbwaiterPhase::Travelling
			&& unit->getOperation() == accepted, "Cycle released or mis-phased after closing");
		world->advanceTicks(120);
		require(unit->isBusy() && unit->getPhase() == core::DumbwaiterPhase::Opening
			&& unit->getOperation() == accepted, "Cycle released or mis-phased on arrival");
		world->advanceTicks(47);
		require(unit->isBusy() && stateOf(*world, accepted) == DeviceOperationState::Running,
			"Cycle completed before the arrival shutter fully opened");
		world->advanceTick();
		require(!unit->isBusy() && unit->getOperation() == core::DeviceOperationId{}
			&& stateOf(*world, accepted) == DeviceOperationState::Succeeded,
			"Cycle ownership did not release on completion");
	}

	// An implicit manual Door is revalidated at activation, not at press:
	// the operation is created Pending by the passing press and only the
	// activation boundary decides whether its requester can still operate
	// the Door from where it stands.
	void manualDoorRevalidationHappensAtActivation(smoke::Context const&)
	{
		for (bool reachable : { false, true })
		{
			core::World world{"Manual door revalidation", 9, 2};
			auto fore = world.addRoom("Fore", 0, 0, 0, 8, 1);
			world.addRoom("Back", 1, 0, 0, 8, 1);
			core::World::CreateDoorOptions options;
			options.activationMode = core::DoorActivationMode::Manual;
			auto created = world.addSectorDoor(0, 0, 3, options);
			world.finishBuild();
			auto const door = created.traversalResource;
			// The far Agent stands at the opposite end of the room from the
			// Door; the near Agent stands beside it.
			auto actor = world.createAgent(reachable ? "Near operator" : "Far operator", fore, 0,
				reachable ? 3.5f : 7.5f);
			world.pauseSimulation();
			require(world.setAgentIndividualObjectUsage(actor, core::ObjectUsage::Arms)
				&& world.setAgentIndividualObjectUsageDistance(actor, 0.5f), "Operator capability setup failed");
			require(world.resumeSimulation(), "Door fixture resume failed");

			core::InteractionBinding binding;
			binding.command = { DeviceCommandType::OpenDoor, {}, true, door };
			auto point = world.createInteractionPoint("Door command", core::SectorId{ fore + 1u },
				world.lookupAgent(actor).entity->getGlobalPosition(), 1.0f, 0.0f, { binding });
			require(bool(point), "Manual Door command binding refused");

			core::SimulationCoordinator coordinator(world);
			auto request = coordinator.requestInteractionWhilePassing(point, actor);
			require(bool(request), "Manual Door passing press refused");
			auto operation = world.lookupInteractionRequest(request).entity->getOperations().front().first;
			require(stateOf(world, operation) == DeviceOperationState::Pending,
				"Manual Door operation was decided before the activation boundary");

			world.advanceTick();
			require(stateOf(world, operation)
				== (reachable ? DeviceOperationState::Running : DeviceOperationState::Rejected),
				"Implicit manual Door revalidation did not decide at the activation boundary");
		}
	}
}

void registerDeviceActivation(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "deviceActivation/boothWindowActivationTick", boothWindowActivatesOnItsOwnTick });
	checks.push_back({ "deviceActivation/boothWindowCompetingTarget", boothWindowCompetingTargetSupersedesSameTick });
	checks.push_back({ "deviceActivation/extensibleCompetingTarget", extensibleOppositeTargetSupersedesSameTick });
	checks.push_back({ "deviceActivation/accessPanelReversalKeepsLeaf", accessPanelReversalKeepsLeafPosition });
	checks.push_back({ "deviceActivation/dumbwaiterBusyOwnership", dumbwaiterBusyOwnershipIsReservedAtAdmission });
	checks.push_back({ "deviceActivation/manualDoorRevalidation", manualDoorRevalidationHappensAtActivation });
}
