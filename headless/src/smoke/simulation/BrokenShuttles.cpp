#include "Checks.h"
#include "BrokenShuttleFixture.h"

namespace
{
	using namespace broken_shuttle;
	using smoke::require;

	void freezeAndRecover()
	{
		Scene scene;
		auto secondId = scene.world->createAgent("Passenger B", scene.secondOrigin, 0, 1.5f);
		auto second = scene.world->lookupAgent(secondId).entity;
		scene.start(); scene.start(second, scene.secondGoal);
		scene.until([&] { auto state = scene.snapshot(); return state.liftMoving && state.liftPosition > 0.5f; });
		auto before = scene.snapshot();
		require(before.occupantCount == 2 && before.shuttleCarriages[0].occupantCount == 1
			&& before.shuttleCarriages[1].occupantCount == 1, "Fixture did not fill independent Carriages");
		auto firstPosition = scene.agent->getGlobalPosition(), secondPosition = second->getGlobalPosition();
		auto firstPath = scene.agent->getPath(), secondPath = second->getPath();
		require(scene.world->setShuttleBroken(scene.made.traversalResource, true), "Live break refused");
		for (uint32_t slot = 0; slot < scene.made.doors.size(); ++slot)
			require(scene.door(slot)->isBroken() && !scene.door(slot)->requestOpen()
				&& !scene.world->setDoorBroken(scene.made.doors[slot].traversalResource, false), "Owned Door operated/independently restored");
		scene.world->advanceTicks(1800);
		auto frozen = scene.snapshot();
		require(frozen.shuttleBroken && !frozen.liftAcceptingBoarders && frozen.liftPosition == before.liftPosition
			&& frozen.occupantCount == 2 && frozen.liftScheduledStops == before.liftScheduledStops
			&& frozen.shuttleCarriages[0].occupantCount == 1 && frozen.shuttleCarriages[1].occupantCount == 1
			&& scene.agent->getGlobalPosition() == firstPosition && second->getGlobalPosition() == secondPosition
			&& scene.agent->getPath() == firstPath && second->getPath() == secondPath, "Broken lost capacity/journey or moved passenger");
		auto remembered = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(remembered && remembered->broken && !remembered->atStop && !remembered->doorsOpen
			&& remembered->position == before.liftPosition, "Passenger did not remember frozen physical condition");
		scene.world->setShuttleBroken(scene.made.traversalResource, false);
		require(scene.snapshot().liftPosition == before.liftPosition && scene.agent->getGlobalPosition() == firstPosition, "Restore teleported vehicle/passenger");
		scene.until([&] { return scene.agent->getState() == core::Agent::State::Idle && second->getState() == core::Agent::State::Idle; });
		require(scene.agent->getSector()->getIndex() == scene.top && second->getSector()->getIndex() == scene.secondDestination
			&& scene.agent->getState() == core::Agent::State::Idle && second->getState() == core::Agent::State::Idle
			&& scene.snapshot().occupantCount == 0, "Restored retained journeys failed to complete");
		// Fresh calls and journeys still work after recovery.
		scene.start(scene.agent, scene.bottomGoal);
		scene.until([&] { return scene.agent->getState() == core::Agent::State::Idle; });
		require(scene.agent->getSector()->getIndex() == scene.bottom && scene.snapshot().occupantCount == 0, "Fresh return journey failed");
	}

	void doorSafety()
	{
		for (int phase = 0; phase < 3; ++phase)
		{
			Scene scene; scene.start();
			scene.until([&]
			{
				auto state = scene.snapshot(); auto door = scene.door(4);
				return state.occupantCount == 1 && !state.liftMoving && state.liftCurrentStop == 1
					&& (phase != 2 ? door->getOpenPercentage() > 0 && door->getOpenPercentage() < 1 : door->isOpen());
			});
			// Shuttle Doors start moving in the arrival tick. Close them through
			// their physical API before breakage to cover aligned, fully closed Doors.
			if (phase == 0)
				for (uint32_t slot = 4; slot < scene.made.doors.size(); ++slot)
				{ scene.door(slot)->requestClose(); scene.door(slot)->update(10.0f); }
			auto position = scene.snapshot().liftPosition;
			std::vector<float> apertures;
			for (uint32_t slot = 0; slot < scene.made.doors.size(); ++slot) apertures.push_back(scene.door(slot)->getOpenPercentage());
			auto newcomerId = scene.world->createAgent("No new boarder", scene.top, 0, 1.5f);
			auto newcomer = scene.world->lookupAgent(newcomerId).entity;
			scene.start(newcomer, scene.bottomGoal); // Pre-break, stale boarding Path.
			scene.world->setShuttleBroken(scene.made.traversalResource, true);
			require(!scene.world->isTransportLocallyBoardable(scene.made.doors[4].traversalResource, { 20, 0 }), "Broken advertised boarding");
			scene.world->advanceTicks(600);
			for (uint32_t slot = 0; slot < apertures.size(); ++slot)
				require(scene.door(slot)->getOpenPercentage() == apertures[slot], "Broken moved an owned Door");
			require(scene.snapshot().liftPosition == position && newcomer->getSector()->getIndex() == scene.top
				&& newcomer->getState() == core::Agent::State::Idle, "Broken moved vehicle or boarded stale Path");
			if (phase != 2) require(scene.snapshot().occupantCount == 1 && scene.agent->getSector()->getIndex() == scene.owner, "Closed/partial Door let passenger escape");
			else require(scene.snapshot().occupantCount == 0 && scene.agent->getSector()->getIndex() == scene.top, "Already-open Door prevented alighting");
			scene.world->setShuttleBroken(scene.made.traversalResource, false); scene.world->advanceTicks(1800);
			require(scene.agent->getState() == core::Agent::State::Idle && scene.agent->getSector()->getIndex() == scene.top
				&& scene.snapshot().occupantCount == 0, "Frozen Door did not resume safely");
		}
		// An open Door in Carriage A must not let Carriage B escape through
		// its own closed Doors or steal A's capacity/selected alighting Door.
		Scene mixed;
		auto secondId = mixed.world->createAgent("Passenger B", mixed.secondOrigin, 0, 1.5f);
		auto second = mixed.world->lookupAgent(secondId).entity;
		mixed.start(); mixed.start(second, mixed.secondGoal);
		mixed.until([&] { auto state = mixed.snapshot(); return state.occupantCount == 2
			&& state.liftCurrentStop == 1 && !state.liftMoving && mixed.door(4)->isOpen(); });
		for (uint32_t slot : { 6u, 7u }) { mixed.door(slot)->requestClose(); mixed.door(slot)->update(10.0f); }
		mixed.world->setShuttleBroken(mixed.made.traversalResource, true); mixed.world->advanceTicks(600);
		require(mixed.agent->getSector()->getIndex() == mixed.top && second->getSector()->getIndex() == mixed.owner
			&& mixed.snapshot().shuttleCarriages[0].occupantCount == 0 && mixed.snapshot().shuttleCarriages[1].occupantCount == 1,
			"Carriage borrowed another Carriage's open Door/capacity");
		mixed.world->setShuttleBroken(mixed.made.traversalResource, false);
		mixed.until([&] { return second->getState() == core::Agent::State::Idle; });
		require(second->getSector()->getIndex() == mixed.secondDestination && mixed.snapshot().occupantCount == 0, "Mixed Door recovery lost journey");

		Scene selecting; selecting.start(); core::DeviceOperationId pending;
		selecting.until([&]
		{
			for (auto const& operation : selecting.world->getSimulationSnapshotView().deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SelectShuttleDestination
					&& operation.state == core::DeviceOperationState::Pending) { pending = operation.id; return true; }
			return false;
		});
		selecting.world->setShuttleBroken(selecting.made.traversalResource, true);
		require(selecting.world->lookupDeviceOperation(pending).entity->getState() == core::DeviceOperationState::Failed, "Pending selector did not fail");
		auto request = selecting.world->requestInteraction(selecting.made.interiorSelector, selecting.id);
		auto interaction = selecting.world->lookupInteractionRequest(request);
		require(interaction && !interaction.entity->getOperations().empty(), "Broken command did not expose outcome");
		require(selecting.world->lookupDeviceOperation(interaction.entity->getOperations().front().first).entity->getState() == core::DeviceOperationState::Failed, "New selector accepted while Broken");
		selecting.world->advanceTicks(900);
		require(selecting.snapshot().occupantCount == 1 && !selecting.snapshot().liftMoving, "Broken selector lost passenger or started journey");
		selecting.world->setShuttleBroken(selecting.made.traversalResource, false);
		selecting.until([&] { return selecting.agent->getState() == core::Agent::State::Idle; });
		require(selecting.agent->getSector()->getIndex() == selecting.top && selecting.snapshot().occupantCount == 0, "Fresh selector after restoration failed");
		for (bool atOpenStop : { false, true })
		{
			Scene cancelled; cancelled.start();
			cancelled.until([&] { auto state = cancelled.snapshot(); return state.occupantCount == 1
				&& (atOpenStop ? !state.liftMoving && state.liftCurrentStop == 1 && cancelled.door(4)->isOpen()
					: state.liftMoving && state.liftPosition > 0.5f); });
			auto position = cancelled.agent->getGlobalPosition();
			cancelled.world->setShuttleBroken(cancelled.made.traversalResource, true);
			cancelled.agent->clearPath(); cancelled.world->advanceTicks(600);
			require(atOpenStop ? cancelled.snapshot().occupantCount == 0 && cancelled.agent->getSector()->getIndex() == cancelled.top
				: cancelled.snapshot().occupantCount == 1 && cancelled.agent->getGlobalPosition() == position,
				"Cancellation teleported a frozen passenger or blocked safe open-Door alighting");
			cancelled.world->setShuttleBroken(cancelled.made.traversalResource, false);
			cancelled.until([&] { return cancelled.snapshot().occupantCount == 0; });
			require(cancelled.agent->getSector()->getIndex() == cancelled.top, "Recovery lost cancelled passenger's safe exit");
		}
	}

	void localMemory()
	{
		Scene scene;
		scene.id = scene.world->createAgent("Remote observer", scene.remote, 0, 1.5f);
		scene.agent = scene.world->lookupAgent(scene.id).entity;
		scene.world->setShuttleBroken(scene.made.traversalResource, true); scene.world->advanceTicks(30);
		require(!scene.agent->rememberedDeviceCondition(scene.made.traversalResource)
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal)), "Unknown remote break leaked into pathing/memory");
		scene.place(scene.bottomGoal); scene.world->advanceTick();
		auto known = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(known && known->broken && known->atStop && !known->doorsOpen, "Passing observer did not discover unavailable service");
		scene.place(scene.remoteGoal);
		require(!scene.world->getGraph()->calculatePath(scene.agent, scene.goal), "Remembered coupled Shuttle not excluded");
		for (auto const& edge : scene.world->getGraph()->getEdges())
			if (edge->getTraversalResourceId() == scene.made.traversalResource
				|| edge->getTraversalResourceId() == scene.made.doors[0].traversalResource)
				for (bool captured : { false, true })
					require(!scene.facts(*edge, edge->getVertex(1), captured).feasible, "Direct/captured arc ignored remembered Broken Shuttle");
		scene.world->setShuttleBroken(scene.made.traversalResource, false); scene.world->advanceTicks(1800);
		require(scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == known
			&& !scene.world->getGraph()->calculatePath(scene.agent, scene.goal), "Remote repair refreshed stale knowledge");
		scene.place(scene.bottomGoal); scene.world->advanceTick();
		require(!scene.agent->rememberedDeviceCondition(scene.made.traversalResource)->broken
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal)), "Re-observation did not discover restored service");
		scene.place(scene.remoteGoal); auto healthy = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		scene.world->setShuttleBroken(scene.made.traversalResource, true); scene.world->advanceTicks(1800);
		require(scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == healthy
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal)), "Remote break changed stale healthy memory");
		// A non-representative, disconnected carriage approach also observes service.
		auto passingId = scene.world->createAgent("Passing B", scene.secondOrigin, 0, 1.5f);
		scene.world->advanceTick(); auto passing = scene.world->lookupAgent(passingId).entity;
		require(passing->rememberedDeviceCondition(scene.made.traversalResource)->broken
			&& passing->rememberedDeviceCondition(scene.made.doors[2].traversalResource)->broken,
			"Disconnected/non-representative approach did not observe Shuttle");

		Scene revisited(true, true);
		revisited.world->advanceTick();
		auto originalDoor = revisited.made.doors[0].traversalResource;
		auto staleAperture = revisited.agent->rememberedDeviceCondition(originalDoor);
		revisited.place(revisited.remoteGoal);
		revisited.world->setShuttleBroken(revisited.made.traversalResource, false);
		auto otherApproach = revisited.world->getGraph()->getClosestVertexInSector(
			revisited.world->getSector(revisited.secondOrigin).get(), { 6.5f, 0.0f });
		revisited.place(otherApproach); revisited.place(revisited.remoteGoal);
		require(revisited.agent->rememberedDeviceCondition(originalDoor) == staleAperture
			&& !revisited.world->knownTransportCondition(originalDoor, revisited.agent, revisited.agent->getSector())->broken,
			"Fresh coupled-vehicle observation failed to supersede stale failure or remotely refreshed Door aperture");
		for (auto const& edge : revisited.world->getGraph()->getEdges())
			if (edge->getTraversalResourceId() == originalDoor)
				for (bool captured : { false, true })
					require(revisited.facts(*edge, edge->getVertex(1), captured).feasible, "Stale landing failure overrode fresh whole-vehicle restoration");
	}

	void routePlanning()
	{
		Scene unavailable;
		auto landing = unavailable.world->lookupTraversalResource(unavailable.made.doors[0].traversalResource);
		auto call = unavailable.world->requestInteraction(landing.entity->getControls().front(), unavailable.id);
		auto pending = unavailable.world->lookupInteractionRequest(call).entity->getOperations().front().first;
		unavailable.world->setShuttleBroken(unavailable.made.traversalResource, true);
		require(unavailable.world->lookupDeviceOperation(pending).entity->getState() == core::DeviceOperationState::Failed, "Pending landing call did not fail");
		auto rejected = unavailable.world->requestInteraction(landing.entity->getControls().front(), unavailable.id);
		auto operation = unavailable.world->lookupInteractionRequest(rejected).entity->getOperations().front().first;
		require(unavailable.world->lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Failed, "New landing call accepted while Broken");
		unavailable.world->advanceTicks(30); unavailable.world->setShuttleBroken(unavailable.made.traversalResource, false);
		unavailable.world->advanceTicks(30); unavailable.start();
		unavailable.until([&] { return !unavailable.snapshot().admissionQueue.empty(); });
		unavailable.world->setShuttleBroken(unavailable.made.traversalResource, true); unavailable.world->advanceTick();
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
			scene.start();
			require(scene.uses(scene.agent->getPath()), "Fixture preferred wrong initial Path");
			scene.world->setShuttleBroken(scene.made.traversalResource, true); scene.world->advanceTick();
			require(scene.agent->getState() == core::Agent::State::RoutePlanning, "Alternative bypassed mandatory planning");
			scene.world->advanceTicks(6); auto alternative = scene.agent->getPath();
			require(alternative && !scene.uses(alternative), "Replacement did not use alternative");
			scene.world->setShuttleBroken(scene.made.traversalResource, false); scene.world->advanceTick();
			require(scene.agent->getState() == core::Agent::State::RoutePlanning, "Restoration did not start voluntary planning");
			scene.world->advanceTicks(6);
			require(persistence == 1.0f ? scene.agent->getPath() == alternative : scene.uses(scene.agent->getPath()), "Restoration ignored ordinary Route persistence");
		}
	}
}

void registerShuttleCrawling(std::vector<smoke::Check>& checks);

void registerBrokenShuttles(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "shuttles/brokenFreezeAndRecoverPassengers", [](smoke::Context const&) { freezeAndRecover(); } });
	checks.push_back({ "shuttles/brokenStopDoorsAndSelectorSafety", [](smoke::Context const&) { doorSafety(); } });
	checks.push_back({ "shuttles/brokenLocalMemory", [](smoke::Context const&) { localMemory(); } });
	checks.push_back({ "shuttles/brokenRoutePlanning", [](smoke::Context const&) { routePlanning(); } });
	registerShuttleCrawling(checks);
}
