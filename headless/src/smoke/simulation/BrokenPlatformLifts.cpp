#include "Checks.h"
#include "BrokenPlatformLiftFixture.h"
#include <cmath>

namespace
{
	using namespace broken_platform_lift;
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
		auto waitingId = scene.world->createAgent("Capacity waiter", scene.bottom, 0, 2.5f);
		auto waiting = scene.world->lookupAgent(waitingId).entity;
		scene.start(waiting);
		require(scene.world->setLiftBroken(scene.made.traversalResource, true), "Live break refused");
		scene.world->advanceTicks(1800);
		auto frozen = scene.snapshot();
		require(frozen.liftBroken && !frozen.liftAcceptingBoarders && frozen.liftPosition == before.liftPosition
			&& frozen.occupantCount == 2 && frozen.liftScheduledStops == before.liftScheduledStops
			&& scene.agent->getGlobalPosition() == firstPosition && second->getGlobalPosition() == secondPosition
			&& scene.agent->getPath() == path && frozen.capacity == 2
			&& waiting->getGlobalPosition().y == 0 && waiting->getState() == core::Agent::State::Idle,
			"Broken lost capacity/journey, moved passenger or admitted capacity waiter");
		auto remembered = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(remembered && remembered->broken && !remembered->atStop && !remembered->doorsOpen
			&& remembered->position == before.liftPosition, "Passenger did not remember frozen physical condition");
		scene.world->setLiftBroken(scene.made.traversalResource, false);
		require(scene.snapshot().liftPosition == before.liftPosition && scene.agent->getGlobalPosition() == firstPosition, "Restore teleported car/passenger");
		scene.world->advanceTicks(2400);
		require(scene.agent->getGlobalPosition().y == 3 && second->getGlobalPosition().y == 3
			&& scene.agent->getState() == core::Agent::State::Idle && second->getState() == core::Agent::State::Idle
			&& scene.snapshot().occupantCount == 0, "Restored retained journeys failed to complete");
	}

	void stopAndSelectorSafety()
	{
		// Both ground and the selected Walkway remain open for disembarking.
		for (bool descending : { false, true })
		{
			Scene scene;
			if (descending) scene.place(scene.goal);
			auto target = descending ? scene.bottomGoal : scene.goal;
			auto path = scene.world->getGraph()->calculatePath(scene.agent, target);
			require(bool(path), "Missing stop journey"); scene.agent->setPath(path, true);
			scene.until([&] { auto state = scene.snapshot(); return state.occupantCount == 1
				&& !state.liftMoving && state.liftPosition == target->getPosition().y; });
			auto newcomerId = scene.world->createAgent("No new boarder", scene.top,
				descending ? 0 : 3, 7.5f);
			auto newcomer = scene.world->lookupAgent(newcomerId).entity;
			auto stale = scene.world->getGraph()->calculatePath(newcomer, descending ? scene.goal : scene.bottomGoal);
			require(bool(stale), "Missing pre-break Path"); newcomer->setPath(stale, true);
			scene.world->setLiftBroken(scene.made.traversalResource, true);
			require(!scene.world->isTransportLocallyBoardable(scene.made.traversalResource,
				target->getPosition()), "Broken platform advertised boarding");
			scene.world->advanceTicks(600);
			require(scene.snapshot().occupantCount == 0 && scene.agent->getState() == core::Agent::State::Idle
				&& scene.agent->getGlobalPosition() == target->getPosition(), "Broken stopped platform prevented alighting");
			require(newcomer->getGlobalPosition().y == target->getPosition().y
				&& newcomer->getState() == core::Agent::State::Idle, "New passenger boarded Broken platform");
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
		require(selecting.agent->getGlobalPosition().y == 3 && selecting.snapshot().occupantCount == 0, "Fresh selector after restoration failed");
	}

	void permissionsAndCommittedBoarding()
	{
		for (bool descending : { false, true })
		{
			Scene scene;
			if (descending) scene.place(scene.goal);
			auto target = descending ? scene.bottomGoal : scene.goal;
			scene.world->pauseSimulation();
			std::string diagnostic;
			auto key = scene.world->addAccessPermission("Platform operator");
			require(bool(key), diagnostic);
			for (uint32_t stop = 0; stop < 2; ++stop)
				require(scene.world->setLiftDestinationPermissionRequirement(scene.owner, stop, { key },
					&diagnostic, scene.made.lift.index), diagnostic);
			require(scene.world->setInteractionPointPermissionRequirement(scene.made.buttons[descending ? 1 : 0].interactionPoint,
				{ key }, &diagnostic), diagnostic);
			require(!scene.world->getGraph()->calculatePath(scene.agent, target), "Protected journey ignored permissions");
			require(scene.world->grantAgentAccessPermission(scene.id, key, &diagnostic), diagnostic);
			scene.world->resumeSimulation();
			auto path = scene.world->getGraph()->calculatePath(scene.agent, target);
			require(bool(path), "Authorized journey unavailable"); scene.agent->setPath(path, true);
			scene.until([&] { return scene.snapshot().liftMoving; });
			scene.world->setLiftBroken(scene.made.traversalResource, true);
			auto position = scene.agent->getGlobalPosition();
			require(scene.world->setAgentRuntimeAccessPermissionGrant(scene.id, key, false), "Runtime revoke failed");
			scene.world->advanceTicks(900);
			require(scene.snapshot().occupantCount == 1 && scene.agent->getGlobalPosition() == position
				&& scene.agent->getPath() == path, "Permission loss destroyed frozen accepted journey");
			scene.world->setLiftBroken(scene.made.traversalResource, false);
			scene.world->advanceTicks(2400);
			require(scene.agent->getGlobalPosition() == target->getPosition() && scene.snapshot().occupantCount == 0,
				"Accepted passenger could not alight after permission loss");
			require(!scene.world->getGraph()->calculatePath(scene.agent, descending ? scene.goal : scene.bottomGoal),
				"Restoration bypassed destination permission for new journey");
		}

		// A claimed virtual boundary is already a safety commitment. Finish that
		// transfer at the frozen Stop, then hold the passenger until restoration.
		Scene scene; scene.start();
		scene.until([&] { return scene.snapshot().admissionReservationCount == 1; });
		scene.world->setLiftBroken(scene.made.traversalResource, true);
		scene.world->advanceTicks(600);
		require(scene.snapshot().occupantCount == 1 && scene.snapshot().admissionReservationCount == 0
			&& scene.snapshot().liftPosition == 0, "Break interrupted committed virtual boarding");
		scene.world->setLiftBroken(scene.made.traversalResource, false);
		scene.world->advanceTicks(2400);
		require(scene.agent->getGlobalPosition() == scene.goal->getPosition() && scene.snapshot().occupantCount == 0,
			"Committed passenger did not recover");

		// Cancellation while between Stops cannot eject a passenger, whereas
		// cancellation on an aligned Broken platform uses its existing safe exit.
		for (bool between : { false, true })
		{
			Scene cancelled; cancelled.start();
			cancelled.until([&] { auto state = cancelled.snapshot(); return state.occupantCount == 1
				&& (between ? state.liftMoving && state.liftPosition > 0.5f : !state.liftMoving && state.liftCurrentStop == 1); });
			cancelled.world->setLiftBroken(cancelled.made.traversalResource, true);
			auto position = cancelled.agent->getGlobalPosition();
			cancelled.agent->clearPath(); cancelled.world->advanceTicks(600);
			require(cancelled.agent->getGlobalPosition() == position && cancelled.snapshot().occupantCount == (between ? 1u : 0u),
				"Broken cancellation lost safe-stop exit or ejected passenger between Stops");
			cancelled.world->setLiftBroken(cancelled.made.traversalResource, false);
			cancelled.world->advanceTicks(2400);
			require(cancelled.snapshot().occupantCount == 0, "Cancelled passenger did not recover safely");
		}
	}

	void localMemory()
	{
		Scene scene;
		// Start remotely with no knowledge; path search must not see live breakage.
		auto localObserver = scene.agent;
		scene.id = scene.world->createAgent("Remote observer", scene.remote, 0, 0.5f);
		scene.agent = scene.world->lookupAgent(scene.id).entity;
		scene.world->setLiftBroken(scene.made.traversalResource, true); scene.world->advanceTicks(30);
		require(localObserver->rememberedDeviceCondition(scene.made.traversalResource)->broken
			&& !scene.agent->rememberedDeviceCondition(scene.made.traversalResource)
			&& scene.uses(scene.world->getGraph()->calculatePath(scene.agent, scene.goal), scene.made.traversalResource), "Unknown remote break leaked into pathing/memory");
		scene.place(scene.bottomGoal); scene.world->advanceTick();
		auto known = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(known && known->broken && known->atStop && known->doorsOpen, "Passing observer did not discover unavailable service");
		scene.place(scene.remoteGoal);
		require(!scene.world->getGraph()->calculatePath(scene.agent, scene.goal), "Remembered whole Lift not excluded");
		for (auto const& edge : scene.world->getGraph()->getEdges())
			if (edge->getTraversalResourceId() == scene.made.traversalResource)
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
		auto call = unavailable.world->requestInteraction(unavailable.made.buttons.front().interactionPoint, unavailable.id);
		auto pending = unavailable.world->lookupInteractionRequest(call).entity->getOperations().front().first;
		unavailable.world->setLiftBroken(unavailable.made.traversalResource, true);
		require(unavailable.world->lookupDeviceOperation(pending).entity->getState() == core::DeviceOperationState::Failed, "Pending landing call did not fail");
		auto rejected = unavailable.world->requestInteraction(unavailable.made.buttons.front().interactionPoint, unavailable.id);
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

void registerBrokenPlatformLifts(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "platformLifts/brokenFreezeAndRecoverPassengers", [](smoke::Context const&) { freezeAndRecover(); } });
	checks.push_back({ "platformLifts/brokenStopAndSelectorSafety", [](smoke::Context const&) { stopAndSelectorSafety(); } });
	checks.push_back({ "platformLifts/brokenPermissionsAndCommittedBoarding", [](smoke::Context const&) { permissionsAndCommittedBoarding(); } });
	checks.push_back({ "platformLifts/brokenLocalMemory", [](smoke::Context const&) { localMemory(); } });
	checks.push_back({ "platformLifts/brokenRoutePlanning", [](smoke::Context const&) { routePlanning(); } });
}
