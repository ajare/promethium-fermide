#include "Checks.h"
#include "PathFixture.h"
#include "InteractionResults.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
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
		core::World world{ "Broken ordinary Doors", 12, 2 };
		uint32_t front, back, remote;
		core::World::CreateDoorResult made, alternative;
		std::shared_ptr<core::Door> door;
		std::shared_ptr<const core::Vertex> target;
		core::AgentId id;
		core::Agent* agent;
		std::map<uint32_t, std::shared_ptr<const core::Vertex>> markers;

		Scene(core::DoorActivationMode mode = core::DoorActivationMode::Manual,
			core::Door::OpenStyle style = core::Door::OpenStyle::OpenUp, bool alternatives = false)
		{
			world.addLayer();
			front = world.addRoom("Front", 0, 0, 0, 11, 1);
			back = world.addRoom("Back", 1, 0, 0, 11, 1);
			remote = world.addRoom("Remote", 2, 0, 0, 11, 1);
			core::World::CreateDoorOptions options;
			options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			options.openStyle = style;
			made = world.addSectorDoor(0, 0, 2, options);
			door = std::static_pointer_cast<core::DoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
			if (alternatives) alternative = world.addSectorDoor(0, 0, 8);
			world.addSectorDoor(1, 0, 5);
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
			for (auto style : { core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft,
				core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenApart })
				for (float position : { 0.0f, 0.5f, 1.0f })
				{
					Scene scene(mode, style);
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
					require(scene.door->getOpenStyle() == style && scene.door->getActivationMode() == mode,
						"Breakage changed authored activation/style");
					// A Broken vertical opening blocks only when too low to crawl
					// through; horizontal openings fit the Agent width.
					bool admitted;
					if (style == core::Door::OpenStyle::OpenUp)
					{
						auto const aperture = position * scene.door->getSize().y;
						admitted = scene.agent->getTraversalCrawlingDoorClearanceExtent()
							<= aperture + core::Door::ClearanceTolerance;
					}
					else admitted = scene.agent->getWidth()
						<= position * scene.door->getSize().x + core::Door::ClearanceTolerance;
					require(bool(scene.path()) == admitted, "Frozen Door routing disagrees with passage");
					if (admitted)
					{
						scene.agent->setPath(scene.path(), true);
						scene.world.advanceTicks(600);
						require(scene.agent->getSector() == scene.world.getSector(scene.back).get()
							&& scene.agent->getState() == core::Agent::State::Idle, "Broken-open crossing failed mode=" + std::to_string(static_cast<int>(mode))
							+ " state=" + std::to_string(static_cast<int>(scene.agent->getState())));
					}
					require(scene.world.setDoorBroken(scene.made.traversalResource, false), "Restore failed");
					require(scene.door->getOpenPercentage() == position && scene.door->requestOpen(), "Restore reset position/refused request");
					scene.door->update(scene.door->getOpenCloseTime());
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
		justClosing.agent->setPath(justClosing.path(), true); justClosing.world.advanceTicks(600);
		require(justClosing.agent->getSector() == justClosing.world.getSector(justClosing.back).get(), "Frozen full opening refused a crossing");
	}

	void operationsAndAdmittedCrossings()
	{
		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled })
		{
			Scene scene(mode);
			scene.agent->setPath(scene.path(), true);
			for (uint32_t tick = 0; tick < 600 && !scene.door->isOpening(); ++tick) scene.world.advanceTick();
			require(scene.door->isOpening(), "Preparation never began");
			scene.world.setDoorBroken(scene.made.traversalResource, true);
			auto snapshot = scene.world.getSimulationSnapshot();
			require(std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(), [](auto const& operation)
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

			Scene crossing(mode);
			crossing.agent->setPath(crossing.path(), true);
			for (uint32_t tick = 0; tick < 600; ++tick)
			{
				crossing.world.advanceTick();
				if (crossing.agent->getState() == core::Agent::State::TraversingEdge
					&& !crossing.world.getSimulationSnapshot().traversalPermits.empty()
					&& crossing.door->isOpen()) break;
			}
			require(crossing.agent->getState() == core::Agent::State::TraversingEdge && crossing.door->isOpen(), "No admitted crossing");
			crossing.world.setDoorBroken(crossing.made.traversalResource, true);
			crossing.world.advanceTicks(600);
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
				if (edge->getType() != core::EdgeType::Door) continue;
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
		// Frozen passage does not authorize operation or erase approach requirements.
		Scene protectedDoor;
		protectedDoor.world.pauseSimulation();
		auto permission = protectedDoor.world.addAccessPermission("Operate");
		require(protectedDoor.world.setManualDoorPermissionRequirement(protectedDoor.made.traversalResource, { permission }), "Protected Door fixture failed");
		protectedDoor.door->requestOpen(); protectedDoor.door->update(protectedDoor.door->getOpenCloseTime());
		protectedDoor.world.setDoorBroken(protectedDoor.made.traversalResource, true);
		require(!protectedDoor.path(), "Broken-open Door erased Permission adherence");
		require(protectedDoor.world.setAgentIndividualPermissionAdherence(protectedDoor.id, false), "Adherence edit failed");
		require(protectedDoor.path() && !protectedDoor.world.canAgentOpenManualDoor(protectedDoor.made.traversalResource, protectedDoor.id),
			"Broken-open passage granted permission to operate");
		protectedDoor.world.resumeSimulation();
		protectedDoor.agent->setPath(protectedDoor.path(), true); protectedDoor.world.advanceTicks(600);
		require(protectedDoor.agent->getSector() == protectedDoor.world.getSector(protectedDoor.back).get(), "Allowed frozen passage failed runtime admission");
		for (bool alternatives : { false, true })
		{
			Scene scene(core::DoorActivationMode::Manual, core::Door::OpenStyle::OpenUp, alternatives);
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

void registerBrokenDoors(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "brokenDoors/frozenPositionAndCommands", [](smoke::Context const&) { frozenPositionAndCommands(); } });
	checks.push_back({ "brokenDoors/operationsAndAdmittedCrossings", [](smoke::Context const&) { operationsAndAdmittedCrossings(); } });
	checks.push_back({ "brokenDoors/individualLocalMemory", [](smoke::Context const&) { individualLocalMemory(); } });
	checks.push_back({ "brokenDoors/discoveryPlanningAndPersistence", [](smoke::Context const&) { discoveryPlanningAndPersistence(); } });
}
