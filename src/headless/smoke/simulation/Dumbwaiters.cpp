#include "Checks.h"
#include "../support/DumbwaiterFixture.h"
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
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
	void agentOperation(smoke::Context const&)
	{
		for (uint32_t stop : {0u, 1u}) for (float x : {0.249f, 0.25f, 0.5f, 0.75f, 0.751f})
		{
			Fixture f;
			auto actorId = f.world->createAgent("Operator", 0, float(stop), x);
			auto actor = f.world->lookupAgent(actorId).entity;
			auto position = actor->getGlobalPosition(); auto path = actor->getPath();
			auto point = f.world->lookupInteractionPoint(f.unit->getLandingButton(stop)).entity;
			require(point && point->getPosition() == core::Vector2{2.5f, float(stop)}
				&& point->getReach() == 0.25f && point->getDurationTicks() == 1
				&& point->getDumbwaiterOwner() == f.id, "Landing Interaction geometry/ownership incorrect");
			require(!f.world->removeInteractionPoint(f.unit->getLandingButton(stop)), "Owned landing removed independently");
			auto request = f.world->requestDumbwaiterLanding(f.id, stop, actorId);
			bool eligible = x >= 0.25f && x <= 0.75f;
			require(bool(request) == eligible && !f.unit->isBusy(), "Reach boundary or admission activated car incorrectly");
			f.ticks(1);
			require(f.unit->isBusy() == eligible && actor->getGlobalPosition() == position && actor->getPath() == path,
				"Landing press auto-approached or changed route");
			f.ticks(216);
			f.position(eligible ? 1.0f : 0.0f);
		}
		for (unsigned scenario = 0; scenario < 7; ++scenario)
		{
			Fixture f; auto id = f.world->createAgent("Restricted", 0, 0, 0.5f);
			auto actor = f.world->lookupAgent(id).entity;
			f.world->pauseSimulation();
			core::MobilityProfile profile; profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
			if (scenario == 0) actor->setActive(false);
			if (scenario == 1) require(f.world->setAgentIndividualMobilityProfile(id, profile), "Buttons edit failed");
			if (scenario == 2)
			{
				profile.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
				require(f.world->setAgentIndividualMobilityProfile(id, profile), "Last-resort edit failed");
			}
			if (scenario >= 3)
			{
				auto registry = core::AgentTagRegistry::create(); auto tag = registry->addAgentTag("no-buttons");
				require(registry->addAgentTagMobilityProfile(tag) && registry->setAgentTagMobilityProfile(tag, profile), "Tag fixture failed");
				f.world->attachAgentTagRegistry("dumbwaiter.tags.yaml", registry);
				require(f.world->assignAgentTag(id, tag), "Tag assignment failed");
				if (scenario >= 4) require(f.world->setAgentIndividualMobilityProfile(id, core::MobilityProfile{}), "Override failed");
				if (scenario == 5) require(f.world->setAgentIndividualMobilityProfile(id, std::nullopt), "Override removal failed");
				if (scenario == 6) require(registry->setAgentTagMobilityProfile(tag, {}), "Tag clearing failed");
			}
			f.world->resumeSimulation();
			auto request = f.world->requestDumbwaiterLanding(f.id, 0, id);
			bool allowed = scenario == 2 || scenario == 4 || scenario == 6;
			require(bool(request) == allowed, "Effective Buttons precedence ignored");
			f.ticks(2); require(f.unit->isBusy() == allowed, "Buttons refusal changed cycle");
		}
		for (bool otherLocation : {false, true})
		{
			Fixture f; f.world->pauseSimulation(); f.world->addLayer();
			auto other = f.world->addRoom("Departure", 2, 0, 2, 1, 2); f.world->finishBuild();
			auto id = f.world->createAgent("Departing before press", 0, 0, 0.5f);
			f.world->resumeSimulation(); auto request = f.world->requestDumbwaiterLanding(f.id, 0, id);
			f.world->pauseSimulation(); auto actor = f.world->lookupAgent(id).entity;
			std::const_pointer_cast<core::Sector>(f.world->getSector(0))->exitAgent(actor);
			std::const_pointer_cast<core::Sector>(f.world->getSector(otherLocation ? other : 0))->enterAgent(actor, 0, otherLocation ? 0.5f : 0.8f);
			auto position = actor->getGlobalPosition(); f.world->resumeSimulation(); f.ticks(1);
			require(f.world->lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Cancelled,
				"Paused departure did not cancel landing press");
			f.ticks(250); f.position(0); f.progress(0, 1);
			require(actor->getGlobalPosition() == position && !f.unit->isBusy(), "Departed Agent auto-approached or operated later");
		}
		// The same coordinates in another Location do not authorize a press.
		{
			Fixture f; f.world->pauseSimulation();
			f.world->addLayer();
			auto other = f.world->addRoom("Other Location", 2, 0, 2, 1, 2);
			f.world->finishBuild(); auto id = f.world->createAgent("Wrong Location", other, 0, 0.5f);
			f.world->resumeSimulation();
			require(!f.world->requestDumbwaiterLanding(f.id, 0, id), "Wrong Location pressed landing");
			f.ticks(3); f.position(0);
		}
	}

	void agentPermissions(smoke::Context const&)
	{
		for (unsigned scenario = 0; scenario < 14; ++scenario)
		{
			Fixture f; auto id = f.world->createAgent("Protected", 0, 0, 0.5f);
			auto actor = f.world->lookupAgent(id).entity;
			f.world->pauseSimulation();
			auto a = f.world->addAccessPermission("A"), b = f.world->addAccessPermission("B");
			auto set = f.world->addPermissionSet("Operators");
			require(f.world->setPermissionSetAccessPermission(set, b, true), "Set fixture failed");
			require(f.world->setInteractionPointPermissionRequirement(f.unit->getLandingButton(0), {a,b}), "Requirements failed");
			require(f.world->getInteractionPointPermissionRequirement(f.unit->getLandingButton(1)).empty(), "Requirements leaked to other landing");
			require(f.world->setAgentIndividualPermissionAdherence(id, false), "Adherence fixture failed");
			if (scenario != 0) require(f.world->grantAgentAccessPermission(id, a), "Direct grant failed");
			if (scenario >= 2)
			{
				if (scenario == 2) require(f.world->grantAgentAccessPermission(id, b), "All direct grant failed");
				else require(f.world->setAgentPermissionSetAssignment(id, set, true), "Set assignment failed");
			}
			f.world->resumeSimulation();
			auto request = f.world->requestDumbwaiterLanding(f.id, 0, id);
			require(bool(request), "Authorization outcome not queryable");
			if (scenario < 2)
			{
				require(f.world->lookupInteractionRequest(request).entity->getResult() == core::InteractionResult::Rejected
					&& f.world->lookupInteractionRequest(request).entity->getMissingPermissions().size() == (scenario == 0 ? 2u : 1u),
					"All-of authorization ignored");
				f.ticks(3); f.position(0); require(!f.unit->isBusy(), "Unauthorized press changed target"); continue;
			}
			if (scenario >= 4 && scenario <= 8)
			{
				f.world->pauseSimulation();
				if (scenario == 4) require(f.world->setAgentRuntimeAccessPermissionGrant(id, a, false), "Pre-press loss failed");
				if (scenario == 5) require(f.world->setAgentRuntimePermissionSetAssignment(id, set, false), "Pre-press set loss failed");
				if (scenario == 6) actor->setActive(false);
				if (scenario == 7)
				{
					core::MobilityProfile profile; profile.set(core::TraversalKind::Buttons, core::MobilityUse::CannotUse);
					require(f.world->setAgentIndividualMobilityProfile(id, profile), "Paused Buttons loss failed");
				}
				if (scenario == 8)
				{
					auto extra = f.world->addAccessPermission("Tightened");
					require(f.world->setInteractionPointPermissionRequirement(f.unit->getLandingButton(0), {a,b,extra}), "Tightening failed");
				}
				f.world->resumeSimulation(); f.ticks(1);
				auto result = f.world->lookupInteractionRequest(request).entity->getResult();
				require(result == core::InteractionResult::Cancelled || result == core::InteractionResult::Rejected,
					"Paused eligibility change did not cancel/reject");
				f.ticks(250); f.position(0); f.progress(0, 1); require(!f.unit->isBusy(), "Refused request became late cycle"); continue;
			}
			f.ticks(1); require(f.unit->isBusy(), "Eligible activation refused");
			auto operation = f.unit->getOperation();
			if (scenario >= 9)
			{
				f.world->pauseSimulation();
				if (scenario == 9) actor->setActive(false);
				if (scenario == 10)
				{
					auto sector = std::const_pointer_cast<core::Sector>(f.world->getSector(0));
					sector->exitAgent(actor); sector->enterAgent(actor, 0, 0.8f);
				}
				if (scenario == 13) require(f.world->setAgentRuntimeAccessPermissionGrant(id, a, false), "Post-press direct loss failed");
				if (scenario == 11) require(f.world->setAgentRuntimePermissionSetAssignment(id, set, false), "Post-press loss failed");
				if (scenario == 12)
				{
					f.world->resumeSimulation(); f.ticks(80); f.world->pauseSimulation();
					auto position = f.unit->getCarPosition(); auto phase = f.unit->getPhase();
					auto extra = f.world->addAccessPermission("Tightened");
					require(f.world->setInteractionPointPermissionRequirement(f.unit->getLandingButton(0), {extra}), "Post-press edit failed");
					require(f.unit->getCarPosition() == position && f.unit->getPhase() == phase && f.unit->getOperation() == operation,
						"Permission-only edit reset cycle progress");
				}
				f.world->resumeSimulation();
			}
			while (f.unit->isBusy()) f.ticks(1);
			f.position(1); f.progress(1, 1);
			require(f.world->lookupDeviceOperation(operation).entity->getState() == DeviceOperationState::Succeeded,
				"Accepted cycle interrupted by eligibility change");
		}
	}

	void agentRacesAndLifecycle(smoke::Context const&)
	{
		for (unsigned repeat = 0; repeat < 3; ++repeat) for (unsigned race = 0; race < 3; ++race)
		{
			Fixture f; auto a = f.world->createAgent("Lower", 0, 0, 0.5f);
			auto b = f.world->createAgent("Upper", 0, 1, 0.5f);
			auto first = f.world->requestDumbwaiterLanding(f.id, 1, b);
			auto second = f.world->requestDumbwaiterLanding(f.id, 0, a);
			core::DeviceOperationId user;
			if (race == 0) user = f.world->pressDumbwaiterLanding(f.id, 1);
			f.ticks(1);
			if (race == 1) user = f.world->pressDumbwaiterLanding(f.id, 1);
			require(f.unit->isBusy(), "Contending presses did not reserve cycle");
			if (race == 0)
				require(f.unit->getOperation() == user, "Earlier user activation lost admission");
			else
			{
				auto op = f.world->lookupInteractionRequest(second).entity->getOperations().front().first;
				require(f.unit->getOperation() == op, "Point-order simultaneous admission not deterministic");
				if (race == 1) require(f.world->lookupDeviceOperation(user).entity->getState() == DeviceOperationState::Rejected, "Busy user won race");
			}
			require(f.world->lookupInteractionRequest(first).entity->getResult() == core::InteractionResult::Rejected,
				"Opposite pending press not refused");
			f.ticks(500); f.position(1); f.progress(1, 1);
			require(!f.unit->isBusy() && f.world->getSimulationSnapshot().interactionRequests.empty()
				&& f.world->getSimulationSnapshot().deviceOperations.empty(), "Refused press replayed or leaked ownership");
		}
		for (unsigned change = 0; change < 4; ++change) for (unsigned elapsed : {0u, 2u})
		{
			Fixture f; auto actor = f.world->createAgent("Outstanding", 0, 0, 0.5f);
			auto point = f.unit->getLandingButton(0);
			auto request = f.world->requestDumbwaiterLanding(f.id, 0, actor);
			auto operation = f.world->lookupInteractionRequest(request).entity->getOperations().front().first;
			f.ticks(elapsed); f.world->pauseSimulation();
			if (change == 0) f.world->resetSimulation();
			if (change == 1) require(f.world->removeDumbwaiter(f.id), "Owner deletion failed");
			if (change == 2) require(f.world->configureDumbwaiter(f.id, {1, 1}), "Configuration cancellation failed");
			if (change == 3) { f.world->addRoom("Structural", 0, 0, 0, 1, 1); f.world->finishBuild(); }
			if (auto live = f.world->lookupInteractionRequest(request); live)
				require(live.entity->getResult() != core::InteractionResult::Pending, "Structural edit retained live device request");
			if (auto live = f.world->lookupDeviceOperation(operation); live)
				require(live.entity->getState() == DeviceOperationState::Cancelled, "Cancellation outcome not observable");
			if (change == 1) require(!f.world->lookupInteractionPoint(point), "Removed device retained point handle");
			f.world->resumeSimulation(); f.ticks(250);
			require(!f.world->lookupInteractionRequest(request) && !f.world->lookupDeviceOperation(operation), "Cancelled work not retired");
		}
	}

	void surroundingEdits()
	{
		using namespace dumbwaiter_fixture;
		for (unsigned phaseTicks : {0u, 12u, 80u, 180u})
		for (unsigned action = 0; action < 12; ++action)
		{
			auto world = make(0, action == 0 || action == 3 || action == 4 || action == 9, 2);
			world->addLayer();
			auto id = world->addDumbwaiter(2, 0, 2, {1, 2});
			auto unrelated = world->addRoom("Unaffected", 3, 2, 4, 1, 1);
			world->finishBuild();
			auto actor = world->createAgent("Unaffected", unrelated, 0, 0.5f);
			auto permission = world->addAccessPermission("Runtime grant");
			world->setAgentRuntimeAccessPermissionGrant(actor, permission, true);
			world->setAgentIndividualWaitingAversion(actor, 1.4f);
			auto unit = world->lookupDumbwaiter(id);
			world->setInteractionPointPermissionRequirement(unit->getLandingButton(0), {permission});
			world->setInteractionPointPermissionRequirement(unit->getLandingButton(1), {permission});
			auto oldButton = unit->getLandingButton(0), oldUpperButton = unit->getLandingButton(1);
			auto oldShutter = unit->getAperture(0)->getDeviceId();
			auto operation = world->pressDumbwaiterLanding(id, 0);
			world->resumeSimulation(); require(world->advanceTicks(phaseTicks), "Structural phase setup failed"); world->pauseSimulation();
			world->consumeSimulationEvents();
			auto authored = yaml(*world); auto position = unit->getCarPosition(); auto phase = unit->getPhase();
			auto invalid = world->planResizeLocation(0, 2, 0, 9, 2);
			require(!invalid.valid, "Invalid surrounding plan accepted");
			bool refused = false; try { world->applyLocationEdit(invalid); } catch (std::exception const&) { refused = true; }
			require(refused && yaml(*world) == authored && unit->getCarPosition() == position
				&& unit->getPhase() == phase && unit->getOperation() == operation
				&& world->consumeSimulationEvents().empty(), "Rejected surrounding edit mutated/cancelled work");
			bool survives = action == 4 || action == 7 || action == 8 || action == 9;
			std::vector<core::SimulationEvent> events;
			if (action == 0) world->applyWalkwayEdit(world->planRemoveSectorWalkway(0, 0));
			if (action == 1 || action == 2) world->applyLocationEdit(world->planRemoveLocation(action - 1));
			if (action == 3) world->applyLocationEdit(world->planResizeLocation(0, 2, 0, 1, 1));
			if (action == 4) world->applyLocationEdit(world->planResizeLocation(0, 2, 0, 2, 2));
			if (action == 5) world->applyDeleteLevel(world->planDeleteLevel(0));
			if (action == 6) world->applyDeleteLayer(world->planDeleteLayer(1));
			if (action == 7) world->applyDeleteLayer(world->planDeleteLayer(0));
			if (action == 8) world->applyDeleteLevel(world->planDeleteLevel(3));
			if (action == 10) world->applyDeleteLevel(world->planDeleteLevel(1));
			if (action == 11) world->applyDeleteLayer(world->planDeleteLayer(2));
			if (action == 9)
			{
				// Pending Agent work is cancelled even when the unit is idle.
				world->configureDumbwaiter(id, {0, 2});
				auto operatorId = world->createAgent("Pending", 0, 0, 0.5f);
				world->grantAgentAccessPermission(operatorId, permission);
				auto request = world->requestDumbwaiterLanding(id, 0, operatorId);
				require(bool(request), "Pending reconciliation fixture refused");
				world->applyLocationEdit(world->planResizeLocation(0, 2, 0, 2, 2));
				require(!world->lookupInteractionRequest(request), "Replay retained pending interaction ownership");
				events = world->consumeSimulationEvents();
				bool cancelledRequest = false;
				for (auto const& event : events)
					if (event.type == core::SimulationEventType::InteractionRequestChanged
						&& event.interactionRequest.id == request && event.interactionRequest.result == core::InteractionResult::Cancelled) cancelledRequest = true;
				require(cancelledRequest, "Pending cancellation not observable after replay");
			}
			bool cancelled = false;
			auto committedEvents = world->consumeSimulationEvents();
			events.insert(events.end(), committedEvents.begin(), committedEvents.end());
			for (auto const& event : events)
				if (event.type == core::SimulationEventType::DeviceOperationChanged
					&& event.deviceOperation.id == operation && event.deviceOperation.state == DeviceOperationState::Cancelled) cancelled = true;
			require(cancelled && !world->lookupInteractionPoint(oldButton) && !world->lookupInteractionPoint(oldUpperButton)
				&& !world->lookupBoothWindow(oldShutter), "Reconciliation lost cancellation or reused stale controls");
			unit = world->lookupDumbwaiter(id);
			require(bool(unit) == survives && world->isTraversalTopologyValid(), "Wrong dependent unit survival");
			require(!world->requestInteraction(oldButton, actor), "Stale landing handle still accepts requests");
			auto restoredActor = world->lookupAgent(actor).entity;
			require(restoredActor && restoredActor->getIndividualWaitingAversion() == std::optional<float>{1.4f}
				&& world->getAgentEffectiveAccessGrants(actor) == std::vector<core::AccessPermissionId>{permission},
				"Structural Dumbwaiter edit lost unrelated runtime grants/Agent");
			if (unit)
			{
				auto initial = action == 9 ? 0u : 1u;
				require(!unit->isBusy() && unit->getCarPosition().y == float(initial)
					&& unit->getAperture(initial)->getProgress() == 1 && unit->getAperture(1-initial)->getProgress() == 0
					&& unit->getNumStops() == 2 && unit->getAperture(0)->getBackLayer() == unit->getAperture(0)->getFrontLayer()+1,
					"Survivor failed authored reset/adjacency");
				for (uint32_t stop = 0; stop < 2; ++stop)
					require(world->getInteractionPointPermissionRequirement(unit->getLandingButton(stop)) == std::vector<core::AccessPermissionId>{permission},
						"Survivor lost landing permission");
				require(bool(world->pressDumbwaiterLanding(id, 0)), "Survivor user control refused");
			}
			else require(!world->pressDumbwaiterLanding(id, 0) && world->getSimulationSnapshot().interactionPoints.empty(), "Removed unit left live controls");
			world->resetSimulation(); world->pauseSimulation();
			require(bool(world->lookupDumbwaiter(id)) == survives, "Canonical replay restored orphan/deleted unit");
		}
	}

	void lifecycle(smoke::Context const&)
	{
		surroundingEdits();
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
	checks.push_back({"dumbwaiters/agentLandingEligibility", agentOperation});
	checks.push_back({"dumbwaiters/agentLandingPermissions", agentPermissions});
	checks.push_back({"dumbwaiters/agentRacesAndOwnership", agentRacesAndLifecycle});
}
