#include "Checks.h"
#include "PathFixture.h"
#include "InteractionResults.h"
#include "core/Agent.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Graph.h"
#include "core/RouteTraversalInputs.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>

namespace
{
	using smoke::require;
	struct Scene
	{
		core::World world{ "Broken Bulkhead Doors", 12, 2 };
		uint32_t front, back, remote;
		core::World::CreateBulkheadDoorResult made;
		core::World::CreateDoorResult alternative;
		std::shared_ptr<core::BulkheadDoor> door;
		std::shared_ptr<const core::Vertex> target;
		core::AgentId id;
		core::Agent* agent;
		std::map<uint32_t, std::shared_ptr<const core::Vertex>> markers;

		Scene(core::DoorActivationMode mode = core::DoorActivationMode::Manual, bool alternatives = false)
		{
			front = world.addRoom("Left", 0, 0, 0, 5, 1);
			back = world.addRoom("Right", 0, 0, 5, 6, 1);
			remote = world.addRoom("Remote", 1, 0, alternatives ? 0 : 5, alternatives ? 11 : 6, 1);
			core::World::CreateBulkheadDoorOptions options;
			options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			made = world.addSectorBulkheadDoor(0, 0, 5, CORE_SIDE_LEFT, options);
			door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
			if (alternatives) alternative = world.addSectorDoor(0, 0, 2);
			world.addSectorDoor(0, 0, 8);
			std::map<uint32_t, std::shared_ptr<core::SectorObject>> objects;
			for (auto sector : { front, back, remote })
			{
				auto marker = world.addSectorMarker(sector, 0, 0.5f);
				objects[sector] = marker.sector->getObject(marker.index);
			}
			world.finishBuild();
			for (auto const& [sector, object] : objects) markers[sector] = world.getGraph()->getVertexForObject(object);
			target = markers[back];
			id = world.createAgent("Observer", front, 0, 0.5f);
			agent = world.lookupAgent(id).entity;
			world.pauseSimulation();
			require(world.setAgentIndividualMinimumRoutePlanningTime(id, 0.1f)
				&& world.setAgentIndividualMaximumRoutePlanningTime(id, 0.1f)
				&& world.setAgentIndividualRoutePersistence(id, 1.0f), "Fixture property authoring failed");
			require(world.resumeSimulation(), "Fixture resume failed");
		}
		void place(uint32_t sector)
		{
			auto route = world.getGraph()->calculatePath(agent, markers[sector]);
			require(bool(route), "Local-memory fixture cannot reach observation Location");
			agent->setPath(route, true);
			for (uint32_t tick = 0; tick < 3600 && agent->getState() != core::Agent::State::Idle; ++tick) world.advanceTick();
			require(agent->getSector() == world.getSector(sector).get(), "Local-memory fixture failed to arrive");
			world.advanceTick();
		}
		std::shared_ptr<core::Path> path() { return world.getGraph()->calculatePath(agent, target); }
	};

	bool uses(std::shared_ptr<core::Path> const& path, core::TraversalResourceId resource)
	{
		return path && std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node)
			{ return node.edge && node.edge->getTraversalResourceId() == resource; });
	}

	void frozenPositionAndCommands()
	{
		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled, core::DoorActivationMode::Unavailable })
			for (float position : { 0.0f, 0.5f, 1.0f })
			{
				Scene scene(mode);
				if (position > 0)
				{
					scene.door->requestOpen();
					scene.door->update(scene.door->getOpenCloseTime() * position);
				}
				require(scene.world.setDoorBroken(scene.made.traversalResource, true), "Live break failed");
				scene.door->update(10.0f);
				require(scene.door->getOpenPercentage() == position && scene.door->isBroken(), "Broken Door moved");
				require(!scene.door->requestOpen() && !scene.door->requestClose()
					&& !scene.door->open() && !scene.door->close() && !scene.door->toggle(), "Broken command succeeded");
				require(scene.door->getActivationMode() == mode,
					"Breakage changed authored activation/style");
				require(bool(scene.path()) == (position == 1), "Frozen Door routing disagrees with passage");
				if (position == 1)
				{
					scene.agent->setPath(scene.path(), true);
					scene.world.advanceTicks(3600);
					require(scene.agent->getSector() == scene.world.getSector(scene.back).get()
						&& scene.agent->getState() == core::Agent::State::Idle, "Broken-open crossing failed mode=" + std::to_string(static_cast<int>(mode))
						+ " state=" + std::to_string(static_cast<int>(scene.agent->getState())));
				}
				require(scene.world.setDoorBroken(scene.made.traversalResource, false), "Restore failed");
				require(scene.door->getOpenPercentage() == position && scene.door->requestOpen(), "Restore reset position/refused request");
				if (!scene.door->isOpen()) scene.door->update(scene.door->getOpenCloseTime());
				require(scene.door->isOpen(), "Restored Door did not resume");
			}

		Scene scene;
		scene.door->requestOpen(); scene.door->update(scene.door->getOpenCloseTime());
		scene.door->requestClose(); scene.door->update(scene.door->getOpenCloseTime() * 0.5f);
		scene.world.setDoorBroken(scene.made.traversalResource, true);
		scene.door->setObstructed(true);
		scene.door->update(10);
		require(scene.door->getOpenPercentage() == 0.5f, "Broken closing position was not frozen");
		scene.world.setDoorBroken(scene.made.traversalResource, false);
		scene.door->update(0.1f);
		require(scene.door->isOpening() && scene.door->getOpenPercentage() == 0.5f, "Restoration ignored obstruction safety");

		// Command direction may change before any physical motion. Fully open
		// Broken admission depends on percentage, not the animation state enum.
		Scene justClosing;
		justClosing.door->requestOpen(); justClosing.door->update(justClosing.door->getOpenCloseTime());
		justClosing.door->requestClose();
		justClosing.world.setDoorBroken(justClosing.made.traversalResource, true);
		require(justClosing.door->isClosing() && justClosing.door->admitsNewCrossings(), "Full opening was confused with motion direction");
		justClosing.agent->setPath(justClosing.path(), true); justClosing.world.advanceTicks(3600);
		require(justClosing.agent->getSector() == justClosing.world.getSector(justClosing.back).get(), "Frozen full opening refused a crossing");
	}

	void concurrentFrozenPassage()
	{
		Scene scene;
		scene.world.removeAgent(scene.id);
		scene.door->requestOpen(); scene.door->update(scene.door->getOpenCloseTime());
		scene.door->requestClose(); // Physical 100% opening, with Closing state.
		scene.world.setDoorBroken(scene.made.traversalResource, true);
		auto leftId = scene.world.createAgent("Left crossing", scene.front, 0, 4.5f);
		auto rightId = scene.world.createAgent("Right crossing", scene.back, 0, 0.5f);
		auto left = scene.world.lookupAgent(leftId).entity;
		auto right = scene.world.lookupAgent(rightId).entity;
		scene.world.advanceTick(); // Both approach memories precede Path selection.
		left->setPath(scene.world.getGraph()->calculatePath(left, scene.markers[scene.back]), true);
		right->setPath(scene.world.getGraph()->calculatePath(right, scene.markers[scene.front]), true);
		bool concurrent = false;
		for (uint32_t tick = 0; tick < 1200; ++tick)
		{
			scene.world.advanceTick();
			auto snapshot = scene.world.getSimulationSnapshot();
			concurrent = concurrent || snapshot.traversalPermits.size() >= 2;
		}
		require(concurrent && left->getSector() == scene.world.getSector(scene.back).get()
			&& right->getSector() == scene.world.getSector(scene.front).get()
			&& left->getState() == core::Agent::State::Idle && right->getState() == core::Agent::State::Idle,
			"Frozen-open Bulkhead passage was not concurrent and bidirectional");
		require(scene.door->getOpenLeaseCount() == 0 && scene.world.getSimulationSnapshot().traversalPermits.empty(),
			"Concurrent frozen passage leaked ownership");
	}

	void automaticPresenceWithoutPath()
	{
		for (bool right : { false, true })
		{
			Scene scene(core::DoorActivationMode::Automatic);
			scene.world.setDoorBroken(scene.made.traversalResource, true);
			auto bystander = scene.world.createAgent("Sensor bystander",
				right ? scene.back : scene.front, 0, right ? 0.3f : 4.7f);
			scene.world.advanceTicks(120);
			require(scene.door->isClosed() && scene.door->getOpenPercentage() == 0,
				"Automatic presence operated a Broken Bulkhead Door");
			auto memory = scene.world.lookupAgent(bystander).entity->rememberedDeviceCondition(scene.made.traversalResource);
			require(memory && memory->broken && memory->position == 0,
				"Idle approach-side bystander missed condition");
			scene.world.setDoorBroken(scene.made.traversalResource, false);
			scene.world.advanceTicks(600);
			require(scene.door->isOpen() && !scene.world.lookupAgent(bystander).entity->getPath(),
				"Restoration did not allow path-independent automatic presence");
			require(!scene.world.lookupAgent(bystander).entity->rememberedDeviceCondition(scene.made.traversalResource)->broken,
				"Fresh approach-side observation retained Broken memory");
		}
	}

	void operationsAndAdmittedCrossings()
	{
		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled })
		{
			Scene scene(mode);
			scene.agent->setPath(scene.path(), true);
			for (uint32_t tick = 0; tick < 3600 && !scene.door->isOpening(); ++tick) scene.world.advanceTick();
			require(scene.door->isOpening(), "Preparation never began");
			scene.world.setDoorBroken(scene.made.traversalResource, true);
			auto snapshot = scene.world.getSimulationSnapshot();
			require(mode == core::DoorActivationMode::Automatic || std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(), [](auto const& operation)
				{ return operation.state == core::DeviceOperationState::Failed; }), "Pending operation did not fail immediately");
			auto position = scene.door->getOpenPercentage();
			scene.world.advanceTicks(120);
			require(scene.door->getOpenPercentage() == position && scene.agent->getSector() == scene.world.getSector(scene.front).get(),
				"Partial Door admitted a waiter");

			// A fresh operation fails even before its operator touches the control.
			core::InteractionBinding binding;
			binding.command.type = core::DeviceCommandType::OpenDoor;
			binding.command.traversalResource = scene.made.traversalResource;
			binding.command.desiredState = true;
			auto point = scene.world.createInteractionPoint("Broken request", core::SectorId{ scene.front + 1u },
				scene.agent->getGlobalPosition(), 1, 0, { binding });
			auto interaction = scene.world.requestInteraction(point, scene.id);
			require(bool(interaction), "Operation request not represented");
			auto operation = scene.world.lookupInteractionRequest(interaction).entity->getOperations().front().first;
			require(scene.world.lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Failed,
				"Fresh Broken operation was accepted");
			scene.world.advanceTicks(5);
			require(simulation_smoke::observedInteractionResult(scene.world, interaction) == core::InteractionResult::Failed,
				"Broken interaction did not report failure");
			scene.world.setDoorBroken(scene.made.traversalResource, false);
			auto fresh = scene.world.requestInteraction(point, scene.id);
			require(bool(fresh) && fresh != interaction, "Restoration reused failed interaction");
			scene.world.advanceTicks(600);
			require(simulation_smoke::observedInteractionResult(scene.world, fresh) == core::InteractionResult::Succeeded,
				"Restoration refused a fresh operation");

			Scene crossing(mode);
			crossing.agent->setPath(crossing.path(), true);
			for (uint32_t tick = 0; tick < 3600; ++tick)
			{
				crossing.world.advanceTick();
				if (crossing.agent->getState() == core::Agent::State::TraversingEdge
					&& !crossing.world.getSimulationSnapshot().traversalPermits.empty()
					&& crossing.door->isOpen()) break;
			}
			require(crossing.agent->getState() == core::Agent::State::TraversingEdge && crossing.door->isOpen(), "No admitted crossing");
			crossing.world.setDoorBroken(crossing.made.traversalResource, true);
			crossing.world.advanceTicks(3600);
			require(crossing.agent->getSector() == crossing.world.getSector(crossing.back).get()
				&& crossing.world.getSimulationSnapshot().traversalPermits.empty()
				&& crossing.door->getOpenLeaseCount() == 0, "Breakage interrupted an admitted crossing/leaked ownership");
		}
	}

	void individualLocalMemory()
	{
		Scene scene;
		scene.world.removeAgent(scene.id);
		scene.id = scene.world.createAgent("Remote observer", scene.remote, 0, 0.5f);
		scene.agent = scene.world.lookupAgent(scene.id).entity;
		scene.target = scene.markers[scene.front];
		scene.world.setDoorBroken(scene.made.traversalResource, true);
		require(bool(scene.path()) && !scene.agent->rememberedDeviceCondition(scene.made.traversalResource), "Search revealed unknown remote breakage");
		scene.place(scene.back);
		scene.world.advanceTick(); // Idle bystander, no Path through the observed Door.
		auto memory = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(memory && memory->broken && memory->position == 0, "Passing/idle local observer missed Broken condition");
		scene.place(scene.remote);
		scene.world.setDoorBroken(scene.made.traversalResource, false);
		scene.world.advanceTicks(120);
		require(!scene.path() && scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == memory,
			"Remote restoration refreshed stale memory");
		auto strangerId = scene.world.createAgent("Uninformed", scene.remote, 0, 0.5f);
		require(bool(scene.world.getGraph()->calculatePath(scene.world.lookupAgent(strangerId).entity, scene.target)), "Memory leaked between Agents");
		scene.place(scene.back); scene.world.advanceTick();
		require(!scene.agent->rememberedDeviceCondition(scene.made.traversalResource)->broken && scene.path(), "Fresh back-side restoration observation did not replace memory");
		scene.door->requestOpen(); scene.door->update(scene.door->getOpenCloseTime());
		scene.world.setDoorBroken(scene.made.traversalResource, true); scene.world.advanceTick();
		memory = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		require(memory && memory->broken && memory->position == 1, "Broken-open physical condition was not remembered");
		scene.place(scene.remote);
		scene.world.setDoorBroken(scene.made.traversalResource, false);
		scene.door->requestClose(); scene.door->update(scene.door->getOpenCloseTime());
		scene.world.setDoorBroken(scene.made.traversalResource, true);
		scene.world.advanceTicks(120);
		require(scene.path() && scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == memory,
			"Remembered broken-open passage consulted remote closed state");
		scene.place(scene.back); scene.world.advanceTick();
		require(!scene.path() && scene.agent->rememberedDeviceCondition(scene.made.traversalResource)->position == 0,
			"Fresh unusable condition did not replace stale broken-open memory");

		// Working knowledge must not turn a remote subsequent failure into a notification.
		scene.world.setDoorBroken(scene.made.traversalResource, false); scene.world.advanceTick();
		scene.place(scene.remote);
		auto working = scene.agent->rememberedDeviceCondition(scene.made.traversalResource);
		scene.world.setDoorBroken(scene.made.traversalResource, true); scene.world.advanceTick();
		require(working && !working->broken && scene.path()
			&& scene.agent->rememberedDeviceCondition(scene.made.traversalResource) == working,
			"Remote failure overwrote remembered working condition");
		scene.place(scene.back);
		require(!scene.path(), "Fresh failure observation did not replace working memory");

		// The eager oracle and value-only demand capture agree for local and stale knowledge.
		for (bool local : { false, true })
		{
			scene.place(local ? scene.back : scene.remote);
			for (auto const& edge : scene.world.getGraph()->getEdges())
			{
				if (edge->getType() != core::EdgeType::BulkheadDoor) continue;
				core::RouteDecisionContext context{ scene.agent, {}, scene.world.getRouteChoicePolicy(), scene.agent->getSector(), scene.agent->getWalkSpeed(), &scene.world };
				for (uint32_t side = 0; side < 2; ++side)
				{
					auto eager = edge->getDirectedTraversalFacts(edge->getVertex(side), context);
					auto demand = core::RouteTraversalInputs::capture(*edge, edge->getVertex(side), context).evaluate(context);
					require(eager.feasible == demand.feasible && eager.exclusionReason == demand.exclusionReason, "Condition capture/oracle disagreement");
				}
			}
		}
	}

	void discoveryPlanningAndPersistence()
	{
		for (bool alternatives : { false, true })
		{
			Scene scene(core::DoorActivationMode::Manual, alternatives);
			auto path = scene.path();
			require(uses(path, scene.made.traversalResource), "Fixture did not select direct Door");
			scene.agent->setPath(path, true);
			scene.world.setDoorBroken(scene.made.traversalResource, true);
			scene.world.advanceTick();
			require(scene.agent->getState() == core::Agent::State::RoutePlanning && !scene.agent->getPath(), "Unusable discovery did not enter mandatory planning");
			scene.world.advanceTicks(5);
			if (alternatives)
			{
				require(uses(scene.agent->getPath(), scene.alternative.traversalResource)
					&& !uses(scene.agent->getPath(), scene.made.traversalResource), "Mandatory planning did not choose usable alternative");
				scene.world.setDoorBroken(scene.made.traversalResource, false);
				scene.world.advanceTick();
				require(scene.agent->getState() == core::Agent::State::RoutePlanning, "Restoration did not enter voluntary planning");
				scene.world.advanceTicks(6);
				require(uses(scene.agent->getPath(), scene.alternative.traversalResource), "Voluntary discovery bypassed Route persistence");
			}
			else
			{
				auto events = scene.world.consumeSimulationEvents();
				require(std::count_if(events.begin(), events.end(), [](auto const& event)
					{ return event.type == core::SimulationEventType::RouteLost; }) == 1, "No replacement did not publish exactly one Route loss");
				scene.world.setDoorBroken(scene.made.traversalResource, false);
				scene.world.advanceTicks(120);
				require(scene.agent->getState() == core::Agent::State::Idle, "Restoration invented a deliberate repair-checking trip");
			}
		}
	}
}

void registerBrokenBulkheadDoors(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "brokenBulkheadDoors/concurrentFrozenPassage", [](smoke::Context const&) { concurrentFrozenPassage(); } });
	checks.push_back({ "brokenBulkheadDoors/automaticPresenceWithoutPath", [](smoke::Context const&) { automaticPresenceWithoutPath(); } });
	checks.push_back({ "brokenBulkheadDoors/frozenPositionAndCommands", [](smoke::Context const&) { frozenPositionAndCommands(); } });
	checks.push_back({ "brokenBulkheadDoors/operationsAndAdmittedCrossings", [](smoke::Context const&) { operationsAndAdmittedCrossings(); } });
	checks.push_back({ "brokenBulkheadDoors/individualLocalMemory", [](smoke::Context const&) { individualLocalMemory(); } });
	checks.push_back({ "brokenBulkheadDoors/discoveryPlanningAndPersistence", [](smoke::Context const&) { discoveryPlanningAndPersistence(); } });
}
