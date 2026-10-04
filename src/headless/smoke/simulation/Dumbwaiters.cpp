#include "Checks.h"
#include "../support/DumbwaiterFixture.h"
#include "core/Agent.h"
#include <cmath>

namespace
{
	using smoke::require;
	using core::DumbwaiterPhase;
	using core::DeviceOperationState;
	struct Fixture
	{
		std::shared_ptr<core::World> world = dumbwaiter_fixture::make();
		core::DumbwaiterId id;
		std::shared_ptr<const core::Dumbwaiter> unit;
		Fixture(uint32_t initial = 0, float seconds = 2)
		{
			id = world->addDumbwaiter(1, 0, 2, {initial, seconds});
			world->finishBuild(); unit = world->lookupDumbwaiter(id);
			require(world->resumeSimulation(), "Fixture resume failed"); world->markSaved();
		}
		void ticks(unsigned n) { require(world->advanceTicks(n), "Dumbwaiter physical ticks failed"); }
		void position(float y) { require(std::abs(unit->getCarPosition().y - y) < 0.00001f, "Car speed/position changed"); }
		void progress(uint32_t stop, float amount)
		{ require(std::abs(unit->getAperture(stop)->getProgress() - amount) < 0.00001f, "Shutter speed/interlock changed"); }
		void busy(uint32_t stop)
		{
			auto accepted = unit->getOperation();
			auto refusal = world->pressDumbwaiterLanding(id, stop);
			require(refusal && refusal != accepted
				&& world->lookupDeviceOperation(refusal).entity->getState() == DeviceOperationState::Rejected,
				"Busy press coalesced, buffered, or accepted");
			require(unit->getButtonState(0) == core::DumbwaiterButtonState::Busy
				&& unit->getButtonState(1) == core::DumbwaiterButtonState::Busy, "Both buttons must remain busy");
		}
	};
	void journeys(smoke::Context const&)
	{
		for (unsigned repeat = 0; repeat < 3; ++repeat)
		for (uint32_t initial : {0u, 1u}) for (uint32_t press : {0u, 1u})
		{
			Fixture f(initial);
			auto authored = dumbwaiter_fixture::yaml(*f.world);
			for (uint32_t departure : {initial, 1 - initial})
			{
				auto arrival = 1 - departure;
				auto operation = f.world->pressDumbwaiterLanding(f.id, press);
				require(operation && f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Running
					&& f.unit->getPhase() == DumbwaiterPhase::Closing, "Press did not reserve queryable cycle immediately");
				f.position(float(departure)); f.progress(departure, 1); f.progress(arrival, 0);
				f.busy(press); f.busy(1 - press); // Deterministic same-tick opposite/same landing contention.
				f.ticks(47); f.progress(departure, 1.0f / 48); f.position(float(departure));
				require(f.unit->getPhase() == DumbwaiterPhase::Closing, "Movement started before full close");
				f.busy(0); f.busy(1);
				f.ticks(1); f.progress(0, 0); f.progress(1, 0); f.position(float(departure));
				require(f.unit->getPhase() == DumbwaiterPhase::Travelling, "Extra departure delay");
				f.ticks(1); f.position(float(departure) + (float(arrival) - float(departure)) / 120);
				f.ticks(59); f.position(0.5f); f.progress(0, 0); f.progress(1, 0); f.busy(0); f.busy(1);
				f.ticks(59); f.position(float(departure) + (float(arrival) - float(departure)) * 119 / 120);
				require(f.unit->getPhase() == DumbwaiterPhase::Travelling, "Car arrived early");
				f.ticks(1); f.position(float(arrival)); f.progress(0, 0); f.progress(1, 0);
				require(f.unit->getPhase() == DumbwaiterPhase::Opening
					&& f.unit->getAperture(arrival)->getTargetOpen(), "Arrival did not automatically open, or added a delay");
				f.busy(0); f.busy(1); f.ticks(47); f.progress(arrival, 47.0f / 48); f.position(float(arrival));
				require(f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Running,
					"Operation succeeded before arrival fully opened");
				f.busy(0); f.busy(1); f.ticks(1); f.progress(arrival, 1); f.progress(departure, 0);
				require(f.unit->getPhase() == DumbwaiterPhase::Idle
					&& f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Succeeded
					&& f.unit->getButtonState(arrival) == core::DumbwaiterButtonState::Here
					&& f.unit->getButtonState(departure) == core::DumbwaiterButtonState::Elsewhere, "Arrival completion/status incorrect");
				f.ticks(600); f.progress(arrival, 1); f.position(float(arrival));
				require(!f.world->lookupDeviceOperation(operation) && f.world->getSimulationSnapshot().deviceOperations.empty(),
					"Consumed operation leaked or busy refusal replayed");
			}
			require(!f.world->isModified() && dumbwaiter_fixture::yaml(*f.world) == authored,
				"Runtime press/progress became an authored edit");
			require(f.world->getSimulationSnapshot().traversalResources.empty(), "Object-service cycle created passenger admission");
		}
		for (float seconds : {0.1f, 0.37f, 60.0f})
		{
			Fixture f(0, seconds); auto operation = f.world->pressDumbwaiterLanding(f.id, 1); f.ticks(48);
			auto travelTicks = unsigned(std::ceil(double(seconds) / double(core::World::getFixedTimestep()) - 0.0001));
			f.ticks(1); f.position(core::World::getFixedTimestep() / seconds);
			f.ticks(travelTicks - 2);
			require(f.unit->getPhase() == DumbwaiterPhase::Travelling, "Configured travel ended early");
			f.ticks(1); f.position(1); f.progress(1, 0); f.ticks(48);
			require(f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Succeeded,
				"Configured travel did not finish on fixed-tick boundary");
		}
	}
	void lifecycle(smoke::Context const&)
	{
		for (unsigned phaseTicks : {12u, 80u, 180u}) for (uint32_t initial : {0u, 1u})
		{
			Fixture f(initial); auto operation = f.world->pressDumbwaiterLanding(f.id, 0); f.ticks(phaseTicks);
			auto position = f.unit->getCarPosition(); auto phase = f.unit->getPhase();
			auto a = f.unit->getAperture(0)->getProgress(), b = f.unit->getAperture(1)->getProgress();
			f.world->pauseSimulation(); f.world->update(20);
			require(!f.world->advanceTick() && f.unit->getPhase() == phase && f.unit->getCarPosition() == position,
				"Pause progressed or cancelled cycle");
			f.progress(0, a); f.progress(1, b);
			require(f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Running,
				"Pause lost accepted operation");
			require(f.world->resumeSimulation(), "Resume failed"); f.world->setSimulationTimeScale(2);
			f.world->update(core::World::getFixedTimestep());
			require(f.world->getSimulationTick() == phaseTicks + 2, "Simulation time scaling bypassed fixed ticks");
			f.ticks(216 - phaseTicks - 2);
			require(f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Succeeded,
				"Resumed cycle did not finish at original boundary");
		}
		for (unsigned action = 0; action < 5; ++action) for (unsigned phaseTicks : {0u, 12u, 80u, 180u})
		{
			Fixture f; auto operation = f.world->pressDumbwaiterLanding(f.id, 1); f.ticks(phaseTicks);
			f.world->pauseSimulation(); f.world->consumeSimulationEvents();
			auto before = f.unit->getCarPosition();
			require(!f.world->configureDumbwaiter(f.id, {0, 2}), "Unchanged configuration reported edit");
			bool refused = false;
			try { f.world->configureDumbwaiter(f.id, {2, 2}); } catch (std::exception const&) { refused = true; }
			require(refused && f.unit->getCarPosition() == before && f.unit->getOperation() == operation
				&& f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Running,
				"Refused/unchanged configuration reset accepted journey");
			if (action == 0) f.world->resetSimulation();
			if (action == 1) require(f.world->configureDumbwaiter(f.id, {1, 2}), "Initial Stop edit refused");
			if (action == 2) require(f.world->configureDumbwaiter(f.id, {0, 0.5f}), "Timing edit refused");
			if (action == 3) require(f.world->removeDumbwaiter(f.id), "Deletion refused");
			if (action == 4) { f.world->addRoom("Unrelated", 0, 0, 0, 1, 1); f.world->finishBuild(); }
			bool cancelled = false;
			for (auto const& event : f.world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DeviceOperationChanged
					&& event.deviceOperation.id == operation && event.deviceOperation.state == DeviceOperationState::Cancelled) cancelled = true;
			require(cancelled, "Lifecycle cancellation not observable through operation events");
			f.unit = f.world->lookupDumbwaiter(f.id);
			if (action == 3) require(!f.unit && !f.world->pressDumbwaiterLanding(f.id, 0), "Deleted device accepts stale press");
			else
			{
				require(f.unit && !f.unit->isBusy() && !f.unit->getOperation(), "Lifecycle left stale accepted journey");
				auto initial = action == 1 ? 1u : 0u; f.position(float(initial)); f.progress(initial, 1); f.progress(1 - initial, 0);
				require(f.world->resumeSimulation(), "Lifecycle resume failed"); f.ticks(250);
				f.position(float(initial)); f.progress(initial, 1);
				require(bool(f.world->pressDumbwaiterLanding(f.id, 0)), "Restored device cannot operate");
			}
		}
		// Configuration/deletion/reconciliation of the unit must not reset an
		// unrelated active shutter or Agent, including later Sector identities.
		for (unsigned action = 0; action < 3; ++action)
		{
			Fixture f; f.world->pauseSimulation();
			auto frontIndex = f.world->addRoom("Other front", 0, 0, 4, 1, 1);
			f.world->addRoom("Other back", 1, 0, 4, 1, 1);
			auto booth = std::static_pointer_cast<const core::BoothWindow>(f.world->addBoothWindow(0, 0, 4).object);
			f.world->finishBuild();
			auto actorId = f.world->createAgent("Unaffected", frontIndex, 0, 0.5f);
			auto actor = f.world->lookupAgent(actorId).entity;
			auto permission = f.world->addAccessPermission("Runtime grant");
			require(f.world->setAgentRuntimeAccessPermissionGrant(actorId, permission, true), "Runtime grant setup failed");
			require(f.world->resumeSimulation(), "Unaffected fixture resume failed");
			core::DeviceCommand command; command.type = core::DeviceCommandType::ToggleBoothWindow;
			command.boothWindow = booth->getDeviceId(); auto shutterOp = f.world->submitDeviceCommand(command);
			f.world->pressDumbwaiterLanding(f.id, 0); f.ticks(12); f.world->pauseSimulation();
			if (action == 0) f.world->configureDumbwaiter(f.id, {1, 1});
			if (action == 1) f.world->removeDumbwaiter(f.id);
			if (action == 2) { f.world->addRoom("Structural", 0, 0, 0, 1, 1); f.world->finishBuild(); }
			require(f.world->lookupBoothWindow(booth->getDeviceId()) == booth
				&& std::abs(booth->getProgress() - 0.25f) < 0.00001f
				&& f.world->lookupDeviceOperation(shutterOp).entity->getState() == DeviceOperationState::Running
				&& f.world->lookupAgent(actorId).entity == actor, "Dumbwaiter edit reset unrelated runtime state");
			require(f.world->resumeSimulation(), "Unaffected resume failed"); f.ticks(36);
			require(booth->getProgress() == 1 && f.world->lookupDeviceOperation(shutterOp).entity->getState() == DeviceOperationState::Succeeded,
				"Unrelated shutter operation did not retain progress/outcome");
			// Its owned panel also retains the correct front/back Sector after compaction.
			auto backActor = f.world->createAgent("Back operator", booth->getBackSector()->getIndex(), 0, 0.5f);
			require(bool(f.world->requestInteraction(booth->getPanel(), backActor)), "Unrelated control Sector identity corrupted by deletion");
		}
		Fixture f;
		require(!f.world->pressDumbwaiterLanding({}, 0) && !f.world->pressDumbwaiterLanding(f.id, 2), "Invalid landing allocated work");
		for (uint32_t stop : {0u, 1u})
		{
			core::DeviceCommand shutter; shutter.type = core::DeviceCommandType::ToggleBoothWindow;
			shutter.boothWindow = f.unit->getAperture(stop)->getDeviceId();
			require(!f.world->submitDeviceCommand(shutter), "Independent owned shutter toggle bypassed interlock");
			shutter.type = core::DeviceCommandType::SetBoothWindowState; shutter.desiredState = true;
			require(!f.world->submitDeviceCommand(shutter), "Independent owned shutter target bypassed interlock");
		}
	}
}
void registerDumbwaiters(std::vector<smoke::Check>& checks)
{
	checks.push_back({"dumbwaiters/interlockedCallSendAndTiming", journeys});
	checks.push_back({"dumbwaiters/pauseAndCancellation", lifecycle});
}
