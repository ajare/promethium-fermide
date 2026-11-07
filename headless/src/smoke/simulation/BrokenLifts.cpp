#include "Checks.h"
#include "BrokenLiftFixture.h"
#include <cmath>

namespace
{
	using namespace broken_lift;
	using smoke::require;

	void freezeAndRecover()
	{
		Scene scene;
		auto secondId = scene.world->createAgent("Second passenger", scene.bottom, 0, 1.5f);
		auto second = scene.world->lookupAgent(secondId).entity;
		scene.start(); scene.start(second);
		scene.until([&] { auto state = scene.snapshot(); return state.liftMoving && state.liftPosition > 0.5f; });
		auto before = scene.snapshot();
		require(before.occupantCount == 2, "Fixture did not board both passengers");
		auto firstPosition = scene.agent->getGlobalPosition(), secondPosition = second->getGlobalPosition();
		auto path = scene.agent->getPath();
		require(scene.world->setLiftBroken(scene.made.traversalResource, true), "Live break refused");
		for (uint32_t stop = 0; stop < 2; ++stop)
			require(scene.door(stop)->isBroken() && !scene.door(stop)->requestOpen()
				&& !scene.world->setDoorBroken(scene.made.doors[stop].traversalResource, false), "Owned Door operated/independently restored");
		scene.world->advanceTicks(1800);
		auto frozen = scene.snapshot();
		require(frozen.liftBroken && !frozen.liftAcceptingBoarders && frozen.liftPosition == before.liftPosition
			&& frozen.occupantCount == 2 && frozen.liftScheduledStops == before.liftScheduledStops
			&& scene.agent->getGlobalPosition() == firstPosition && second->getGlobalPosition() == secondPosition
			&& scene.agent->getPath() == path, "Broken lost occupancy/journey or moved passenger");
		auto remembered = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(remembered && remembered->broken && !remembered->atStop && !remembered->doorsOpen
			&& remembered->position == before.liftPosition, "Passenger did not remember frozen physical condition");
		scene.world->setLiftBroken(scene.made.traversalResource, false);
		require(scene.snapshot().liftPosition == before.liftPosition && scene.agent->getGlobalPosition() == firstPosition, "Restore teleported car/passenger");
		scene.world->advanceTicks(2400);
		require(scene.agent->getSector()->getIndex() == scene.top && second->getSector()->getIndex() == scene.top
			&& scene.agent->getState() == core::Agent::State::Idle && second->getState() == core::Agent::State::Idle
			&& scene.snapshot().occupantCount == 0, "Restored retained journeys failed to complete");
	}

	void doorSafety()
	{
		for (int phase = 0; phase < 3; ++phase)
		{
			Scene scene; scene.start();
			scene.until([&]
			{
				auto state = scene.snapshot(); auto door = scene.door(1);
				return state.occupantCount == 1 && !state.liftMoving && state.liftCurrentStop == 1
					&& (phase == 0 ? door->isClosed() : phase == 1
						? door->getOpenPercentage() > 0 && door->getOpenPercentage() < 1 : door->isOpen());
			});
			auto position = scene.snapshot().liftPosition;
			auto aperture = scene.door(1)->getOpenPercentage(); auto carDoor = scene.snapshot().liftCarDoorOpen;
			auto newcomerId = scene.world->createAgent("No new boarder", scene.top, 0, 7.5f);
			auto newcomer = scene.world->lookupAgent(newcomerId).entity;
			auto stalePath = scene.world->getGraph()->calculatePath(newcomer, scene.bottomGoal);
			require(bool(stalePath), "Missing pre-break boarding Path"); newcomer->setPath(stalePath, true);
			scene.world->setLiftBroken(scene.made.traversalResource, true);
			require(!scene.world->isTransportLocallyBoardable(scene.made.doors[1].traversalResource, { 5, 3 }), "Broken open landing advertised boarding");
			scene.world->advanceTicks(600);
			require(scene.door(1)->getOpenPercentage() == aperture && scene.snapshot().liftPosition == position
				&& scene.snapshot().liftCarDoorOpen == carDoor, "Broken moved landing/car Door or car");
			require(newcomer->getSector()->getIndex() == scene.top && newcomer->getState() == core::Agent::State::Idle,
				"New passenger boarded through Broken open Doors/stale Path");
			if (phase != 2) require(scene.snapshot().occupantCount == 1 && scene.agent->getSector()->getIndex() == scene.owner, "Closed/partial Door let passenger escape");
			else require(scene.snapshot().occupantCount == 0 && scene.agent->getSector()->getIndex() == scene.top, "Already-open Doors prevented alighting");
			scene.world->setLiftBroken(scene.made.traversalResource, false);
			scene.world->advanceTicks(1800);
			require(scene.agent->getState() == core::Agent::State::Idle && scene.agent->getSector()->getIndex() == scene.top
				&& scene.snapshot().occupantCount == 0, "Frozen Door did not resume safely");
		}
		// Breaking during a pending onboard selector must fail the operation,
		// not destroy the boarded passenger's intent or exhaust destination retries.
		Scene selecting; selecting.start(); core::DeviceOperationId pending;
		selecting.until([&]
		{
			for (auto const& operation : selecting.world->getSimulationSnapshotView().deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SelectLiftDestination
					&& operation.state == core::DeviceOperationState::Pending) { pending = operation.id; return true; }
			return false;
		});
		selecting.world->setLiftBroken(selecting.made.traversalResource, true);
		require(selecting.world->lookupDeviceOperation(pending).entity->getState() == core::DeviceOperationState::Failed, "Pending selector did not fail");
		auto request = selecting.world->requestInteraction(selecting.made.interiorSelector, selecting.id);
		auto interaction = selecting.world->lookupInteractionRequest(request);
		require(interaction && !interaction.entity->getOperations().empty(), "Broken command did not expose outcome");
		require(selecting.world->lookupDeviceOperation(interaction.entity->getOperations().front().first).entity->getState() == core::DeviceOperationState::Failed, "New selector accepted while Broken");
		selecting.world->advanceTicks(900);
		require(selecting.snapshot().occupantCount == 1 && !selecting.snapshot().liftMoving, "Broken selector lost passenger or started journey");
		selecting.world->setLiftBroken(selecting.made.traversalResource, false); selecting.world->advanceTicks(2400);
		require(selecting.agent->getSector()->getIndex() == selecting.top && selecting.snapshot().occupantCount == 0, "Fresh selector after restoration failed");
	}

	void localMemory()
	{
		Scene scene;
		// Start remotely with no knowledge; path search must not see live breakage.
		scene.id = scene.world->createAgent("Remote observer", scene.remote, 0, 0.5f);
		scene.agent = scene.world->lookupAgent(scene.id).entity;
		scene.world->setLiftBroken(scene.made.traversalResource, true); scene.world->advanceTicks(30);
		require(!scene.agent->rememberedDeviceCondition(scene.made.traversalResource)
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal), scene.made.traversalResource), "Unknown remote break leaked into pathing/memory");
		scene.place(scene.bottomGoal); scene.world->advanceTick();
		auto known = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(known && known->broken && known->atStop && !known->doorsOpen, "Passing observer did not discover unavailable service");
		scene.place(scene.remoteGoal);
		require(!scene.world->getGraph()->calculatePath(scene.agent, scene.goal), "Remembered whole Lift not excluded");
		for (auto const& edge : scene.world->getGraph()->getEdges())
			if (edge->getTraversalResourceId() == scene.made.traversalResource
				|| edge->getTraversalResourceId() == scene.made.doors[0].traversalResource)
				for (bool captured : { false, true })
					require(!scene.facts(*edge, edge->getVertex(1), captured).feasible, "Direct/captured arc ignored remembered Broken Lift");
		scene.world->setLiftBroken(scene.made.traversalResource, false); scene.world->advanceTicks(1800);
		require(scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == known
			&& !scene.world->getGraph()->calculatePath(scene.agent, scene.goal), "Remote repair refreshed stale knowledge");
		scene.place(scene.bottomGoal); scene.world->advanceTick();
		require(!scene.agent->rememberedDeviceCondition(scene.made.traversalResource)->broken
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal), scene.made.traversalResource), "Re-observation did not discover restored service");
		scene.place(scene.remoteGoal); auto healthy = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		scene.world->setLiftBroken(scene.made.traversalResource, true); scene.world->advanceTicks(1800);
		require(scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == healthy
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal), scene.made.traversalResource), "Remote break changed stale healthy memory");
	}

	void routePlanning()
	{
		Scene unavailable;
		auto landing = unavailable.world->lookupTraversalResource(unavailable.made.doors[0].traversalResource);
		auto call = unavailable.world->requestInteraction(landing.entity->getControls().front(), unavailable.id);
		auto pending = unavailable.world->lookupInteractionRequest(call).entity->getOperations().front().first;
		unavailable.world->setLiftBroken(unavailable.made.traversalResource, true);
		require(unavailable.world->lookupDeviceOperation(pending).entity->getState() == core::DeviceOperationState::Failed, "Pending landing call did not fail");
		auto rejected = unavailable.world->requestInteraction(landing.entity->getControls().front(), unavailable.id);
		auto operation = unavailable.world->lookupInteractionRequest(rejected).entity->getOperations().front().first;
		require(unavailable.world->lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Failed, "New landing call accepted while Broken");
		unavailable.world->advanceTicks(30);
		unavailable.world->setLiftBroken(unavailable.made.traversalResource, false);
		unavailable.world->advanceTicks(30);
		unavailable.start();
		unavailable.until([&] { return !unavailable.snapshot().admissionQueue.empty(); });
		unavailable.world->setLiftBroken(unavailable.made.traversalResource, true); unavailable.world->advanceTick();
		require(unavailable.agent->getState() == core::Agent::State::RoutePlanning && !unavailable.agent->getPath()
			&& unavailable.snapshot().admissionQueue.empty() && unavailable.snapshot().admissionReservationCount == 0, "Waiting discovery did not mandate planning/release admission");
		unavailable.world->advanceTicks(120);
		auto events = unavailable.world->consumeSimulationEvents();
		require(unavailable.agent->getState() == core::Agent::State::Idle
			&& std::count_if(events.begin(), events.end(), [](auto const& event) { return event.type == core::SimulationEventType::RouteLost; }) == 1, "Missing replacement did not cause exactly one Route loss");
		for (float persistence : { 0.0f, 1.0f })
		{
			Scene scene(false, true);
			scene.world->pauseSimulation(); scene.world->setAgentIndividualRoutePersistence(scene.id, persistence); scene.world->resumeSimulation();
			scene.start(); require(scene.uses(scene.agent->getPath(), scene.made.traversalResource), "Fixture preferred wrong initial Lift");
			scene.world->setLiftBroken(scene.made.traversalResource, true); scene.world->advanceTick();
			require(scene.agent->getState() == core::Agent::State::RoutePlanning, "Alternative bypassed mandatory planning");
			scene.world->advanceTicks(6);
			auto alternative = scene.agent->getPath();
			require(scene.uses(alternative, scene.alternative.traversalResource), "Replacement did not use other Lift");
			scene.world->setLiftBroken(scene.made.traversalResource, false); scene.world->advanceTick();
			require(scene.agent->getState() == core::Agent::State::RoutePlanning, "Restoration did not start voluntary planning");
			scene.world->advanceTicks(6);
			require(persistence == 1.0f ? scene.agent->getPath() == alternative
				: scene.uses(scene.agent->getPath(), scene.made.traversalResource), "Restoration ignored ordinary Route persistence");
		}
	}
}

void registerLiftCrawling(std::vector<smoke::Check>& checks);

void registerBrokenLifts(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "lifts/brokenFreezeAndRecoverPassengers", [](smoke::Context const&) { freezeAndRecover(); } });
	checks.push_back({ "lifts/brokenStopDoorsAndSelectorSafety", [](smoke::Context const&) { doorSafety(); } });
	checks.push_back({ "lifts/brokenLocalMemory", [](smoke::Context const&) { localMemory(); } });
	checks.push_back({ "lifts/brokenRoutePlanning", [](smoke::Context const&) { routePlanning(); } });
	registerLiftCrawling(checks);
}
