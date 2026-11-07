#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void ordinaryCommands()
	{
		core::World world("Movement commands", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
		auto isolated = world.addRoom("Isolated", 0, 0, 10, 2, 1);
		world.addSectorMarker(room, 0, 8.5f, "End");
		world.addSectorMarker(room, 0, 2.5f, "Other");
		world.addSectorMarker(isolated, 0, 0.5f, "Unreachable");
		world.finishBuild();
		auto markers = world.getMarkerIds();
		auto id = world.createAgent("Walker", room, 0, 0.5f);
		using Status = core::MovementCommandStatus;
		require(world.moveAgentToMarker({}, markers[0]).status == Status::UnknownAgent, "Unknown Agent accepted");
		require(world.moveAgentToMarker(id, {}).status == Status::UnknownMarker, "Unknown Marker accepted");
		world.pauseSimulation();
		require(world.setAgentActive(id, false), "Deactivation refused");
		require(world.moveAgentToMarker(id, markers[0]).status == Status::InactiveAgent, "Inactive Agent accepted");
		require(world.setAgentActive(id, true), "Activation refused");
		world.resumeSimulation();
		world.consumeSimulationEvents();
		require(world.availableAgentActions(markers[0]) == std::vector<std::string>{ "idle" },
			"Ordinary Marker must offer only immutable Idle");
		require(world.availableAgentActions({}).empty(), "Unknown Marker offered Actions");
		require(world.moveAgentToNamedMarker(id, "End", "sit").status == Status::UnavailableAction,
			"Unavailable Action silently fell back to Idle");
		require(world.moveAgentToNamedMarker(id, "Missing").status == Status::UnknownMarker,
			"Unknown authored name accepted");
		require(world.moveAgentToNamedMarker(id, "End").accepted(), "Movement refused");
		require(world.getSimulationSnapshot().agents.front().selectedAction == core::IdleAction,
			"Default Idle was not retained during planning");
		require(world.consumeSimulationEvents().empty(), "Planning invoked arrival synchronously");
		require(world.moveAgentToMarker(id, markers[0], core::IdleAction).status == Status::NoOp,
			"Explicit/default same destination not idempotent");
		require(world.moveAgentToMarker(id, markers[1], core::IdleAction).status == Status::Superseded,
			"Explicit Idle replacement refused");
		world.advanceTick();
		unsigned replacements = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::MovementCancelled)
			{
				++replacements;
				require(event.destinationMarker == markers[0] && event.selectedAction == core::IdleAction
					&& event.movementCancellationReason == core::MovementCancellationReason::Superseded,
					"Replacement outcome lost Action or target");
			}
		require(replacements == 1, "Replacement did not publish exactly one cancellation");
		world.advanceTicks(5);
		world.consumeSimulationEvents();
		require(world.cancelAgentMovement(id).accepted(), "Cancellation refused");
		require(world.consumeSimulationEvents().empty(), "Cancellation completed synchronously");
		world.advanceTick();
		auto events = world.consumeSimulationEvents();
		unsigned cancelled = 0;
		for (auto const& event : events)
			if (event.type == core::SimulationEventType::MovementCancelled)
			{
				++cancelled;
				require(event.destinationMarker == markers[1]
					&& event.movementCancellationReason
						== core::MovementCancellationReason::Explicit
					&& event.tick != 0 && event.sequence != 0,
					"Cancellation event lacked immutable semantic payload");
			}
		require(cancelled == 1, "Cancellation outcome missing or duplicated");
		require(world.cancelAgentMovement(id).status == Status::NoOp, "Idle cancellation not idempotent");
		require(world.moveAgentToMarker(id, markers[1]).accepted(), "Replacement after cancellation refused");
		world.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : world.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Walking destination outcome missing");
		require(world.moveAgentToNamedMarker(id, "Other", core::IdleAction).accepted(), "Already-at-destination command refused");
		require(world.consumeSimulationEvents().empty(), "Same-target arrival fired at acceptance");
		world.advanceTicks(world.lookupAgent(id).entity->getRoutePlanningRemainingTicks());
		reached = 0;
		for (auto const& event : world.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Already-at-destination did not complete");
		require(world.moveAgentToMarker(id, markers[0]).accepted(), "Replanning fixture movement refused");
		world.advanceTicks(5);
		world.pauseSimulation();
		world.finishBuild();
		world.resumeSimulation();
		world.advanceTicks(1000);
		reached = 0;
		for (auto const& event : world.consumeSimulationEvents())
		{
			require(event.type != core::SimulationEventType::RouteLost, "Successful same-destination replan reported route loss");
			reached += event.type == core::SimulationEventType::DestinationReached;
		}
		require(reached == 1, "Same-destination replanning lost the movement goal");
		require(world.moveAgentToMarker(id, markers[2]).accepted(), "Unreachable destination was not accepted as intent");
		world.advanceTicks(world.lookupAgent(id).entity->getRoutePlanningRemainingTicks());
		unsigned lost = 0;
		for (auto const& event : world.consumeSimulationEvents()) lost += event.type == core::SimulationEventType::RouteLost;
		require(lost == 1 && world.getSimulationSnapshot().traversalRequests.empty(), "Initial route loss leaked claims or outcome");
	}

	void stableMarkerRequests()
	{
		for (int scenario = 0; scenario < 3; ++scenario)
		{
			auto const remove = scenario != 0;
			auto const reissue = scenario == 2;
			core::World world("Stable Action target", 10, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
			auto object = world.addSectorMarker(room, 0, 8.5f, "Target");
			world.finishBuild();
			auto marker = world.getMarkerIds().front();
			auto id = world.createAgent("Walker", room, 0, 0.5f);
			world.consumeSimulationEvents();
			require(world.moveAgentToNamedMarker(id, "Target", core::IdleAction).accepted(), "Named request refused");
			world.advanceTicks(2);
			world.pauseSimulation();
			if (remove)
			{
				require(world.removeSectorMarker(room, object.index), "Target deletion refused");
				world.addSectorMarker(room, 0, 2.5f, "Target");
				require(world.getMarkerIds().front() != marker, "Name reuse reused stable identity");
			}
			else require(world.renameMarker(marker, "Renamed"), "Rename refused");
			require(world.rebuildTraversalTopology(), "Target edit rebuild refused");
			if (reissue) require(world.moveAgentToNamedMarker(id, "Target").accepted(), "New named request refused");
			require(world.resumeSimulation(), "Resume refused");
			world.advanceTicks(1800);
			unsigned outcomes = 0;
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled
					|| event.type == core::SimulationEventType::DestinationReached
					|| event.type == core::SimulationEventType::RouteLost)
				{
					++outcomes;
					if (reissue && event.type == core::SimulationEventType::DestinationReached)
					{
						require(event.destinationMarker != marker && event.selectedAction == core::IdleAction,
							"New named request reused the deleted identity");
						continue;
					}
					require(event.destinationMarker == marker && event.selectedAction == core::IdleAction,
						"Accepted identity or Action changed through edit");
					require(remove ? event.type == core::SimulationEventType::MovementCancelled
						&& event.movementCancellationReason == core::MovementCancellationReason::TargetDeleted
						: event.type == core::SimulationEventType::DestinationReached,
						"Rename/deletion outcome incorrect");
				}
			require(outcomes == (reissue ? 2u : 1u), "Target edit lost or duplicated outcome");
			require(world.lookupAgent(id).entity->getGlobalPosition().x == (reissue ? 2.5f : remove ? 0.5f : 8.5f),
				"Name reuse redirected accepted request");
		}
	}

	void cancelDoorCrossing()
	{
		core::World world("Door cancellation", 8, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
		world.addSectorDoor(front, 0, 2, {});
		world.addSectorMarker(back, 0, 6.5f, "End");
		world.finishBuild();
		auto id = world.createAgent("Walker", front, 0, 0.5f);
		auto agent = world.lookupAgent(id).entity;
		auto marker = world.getMarkerIds()[0];
		require(world.moveAgentToMarker(id, marker).accepted(), "Door route refused");
		bool requested = false, completed = false;
		for (unsigned tick = 0; tick < 2000 && !completed; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled) completed = true;
			if (!requested && agent->getState() == core::Agent::State::TraversingEdge)
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					if (request.owner == id && request.edgeType == core::EdgeType::Door && request.permit)
					{
						require(world.cancelAgentMovement(id).accepted(), "Door cancellation refused");
						requested = true;
					}
		}
		require(completed && agent->getSector()->getIndex() == back, "Door crossing was interrupted before commit");
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Door claims leaked");
		require(world.moveAgentToMarker(id, marker).accepted(), "Movement after Door cancellation refused");
		world.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : world.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Door journey did not reach Marker");
	}

	void initialWaypointBeforeLiftCallIsSkipped()
	{
		core::World world("Initial waypoint skip", 6, 2);
		auto lower = world.addCorridor(0, 0, 6);
		auto upper = world.addCorridor(1, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1 };
		world.addLift(1, 0, 2, options);
		uint32_t sourceVertexId, destinationVertexId;
		world.addSectorMarker(upper, 0, 5.5f, &sourceVertexId);
		world.addSectorMarker(lower, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto id = world.createAgent("Passenger", upper, 0, 4.75f);
		auto* agent = world.lookupAgent(id).entity;
		auto graph = world.getGraph();
		auto path = graph->calculatePath(agent,
			graph->getVertexByIdentifier(sourceVertexId),
			graph->getVertexByIdentifier(destinationVertexId));
		require(path && path->nodes.size() >= 2, "Initial-waypoint fixture has no route");
		auto const& first = path->nodes[0].targetVertex;
		auto const& second = path->nodes[1].targetVertex;
		require(first->getSubType() == core::VertexSubType::Marker
			&& second->getSubType() == core::VertexSubType::Interactable
			&& first->getSector()->getLayerIndex() == second->getSector()->getLayerIndex()
			&& first->getPosition().y == second->getPosition().y
			&& first->getPosition().x > 4.75f && second->getPosition().x < 4.75f,
			"Initial-waypoint fixture does not straddle the Agent on one Layer and Level");
		agent->setPath(path, true);

		world.advanceTick();
		require(agent->getGlobalPosition().x < 4.75f,
			"Agent doubled back to an initial Marker instead of heading for the Lift call control");
	}

	void liftCallIsPressedWhilePassing()
	{
		core::World world("Passing Lift call", 6, 2);
		auto lower = world.addCorridor(0, 0, 6);
		auto upper = world.addCorridor(1, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1 };
		world.addLift(1, 0, 2, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(lower, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto id = world.createAgent("Passenger", upper, 0, 4.75f);
		auto* agent = world.lookupAgent(id).entity;
		auto graph = world.getGraph();
		auto path = graph->calculatePath(agent,
			graph->getVertexByIdentifier(destinationVertexId));
		require(path && path->nodes.size() >= 2, "Passing-Lift-call fixture has no route");
		require(path->nodes[0].targetVertex->getSubType() == core::VertexSubType::Interactable
			&& path->nodes[0].targetVertex->getPosition().x == 3.0f
			&& path->nodes[1].targetVertex->getPosition().x == 2.5f,
			"Passing-Lift-call fixture does not put the Button before the Door");
		agent->setPath(path, true);

		float previousX = agent->getGlobalPosition().x;
		float minimumX = previousX;
		bool observedInteraction = false;
		float firstInteractionX = 0.0f;
		bool doubledBack = false;
		for (unsigned tick = 0; tick < 1000 && agent->getSector()->getIndex() == upper; ++tick)
		{
			world.advanceTick();
			auto x = agent->getGlobalPosition().x;
			minimumX = std::min(minimumX, x);
			if (x > previousX + 0.0001f && minimumX < 2.75f) doubledBack = true;
			previousX = x;
			auto snapshot = world.getSimulationSnapshot();
			if (!observedInteraction && std::any_of(snapshot.interactionRequests.begin(),
				snapshot.interactionRequests.end(), [&](auto const& request)
					{ return request.actor == id; }))
			{
				observedInteraction = true;
				firstInteractionX = x;
			}
		}
		require(observedInteraction && firstInteractionX >= 2.85f,
			"Agent did not press the Lift call while passing it");
		require(!doubledBack, "Agent reached the Lift Door and doubled back to its call Button");
	}

	void liftPassengerWalksToExitAlignment()
	{
		core::World world("Lift exit alignment", 8, 2);
		auto lower = world.addCorridor(0, 0, 8);
		auto upper = world.addCorridor(1, 0, 8);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2;
		options.capacity = 2;
		options.stopOffsets = { 0, 1 };
		auto lift = world.addLift(1, 0, 2, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(upper, 0, 7.5f, &destinationVertexId);
		world.finishBuild();

		auto id = world.createAgent("Passenger", lower, 0, 0.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto graph = world.getGraph();
		auto path = graph->calculatePath(agent,
			graph->getVertexByIdentifier(destinationVertexId));
		require(bool(path), "Lift-exit-alignment fixture has no route");
		agent->setPath(path, true);

		auto const liftSector = lift.lift.sector->getIndex();
		auto const maximumStep = agent->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
		float previousX = agent->getGlobalPosition().x;
		bool wasInLift = false;
		bool walkedInside = false;
		bool teleportedInside = false;
		for (unsigned tick = 0; tick < 10000 && agent->getState() != core::Agent::State::Idle; ++tick)
		{
			world.advanceTick();
			auto const inLift = agent->getSector()->getIndex() == liftSector;
			auto const step = std::abs(agent->getGlobalPosition().x - previousX);
			if (wasInLift && inLift)
			{
				walkedInside = walkedInside || step > 0.0001f;
				teleportedInside = teleportedInside || step > maximumStep;
			}
			wasInLift = inLift;
			previousX = agent->getGlobalPosition().x;
		}
		require(walkedInside, "Lift passenger did not approach the exit inside the car");
		require(!teleportedInside,
			"Lift passenger teleported to the Door vertex instead of approaching at walking speed");
		require(agent->getSector()->getIndex() == upper,
			"Lift passenger did not finish the exit journey");
	}

	void shuttleCallIsPressedWhilePassing()
	{
		core::World world("Passing Shuttle call", 12, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
		auto right = world.addRoom("Right", 0, 0, 7, 3, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 7 }, 0 };
		options.capacity = 2;
		options.doorMask = 1;
		world.addShuttle(1, 0, 0, 11, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(right, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto id = world.createAgent("Passenger", left, 0, 2.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto graph = world.getGraph();
		auto path = graph->calculatePath(agent,
			graph->getVertexByIdentifier(destinationVertexId));
		require(path && path->nodes.size() >= 2, "Passing-Shuttle-call fixture has no route");
		require(path->nodes[0].targetVertex->getSubType() == core::VertexSubType::Interactable
			&& path->nodes[0].targetVertex->getPosition().x == 1.0f
			&& path->nodes[1].targetVertex->getPosition().x == 0.5f,
			"Passing-Shuttle-call fixture does not put the Button before the Door");
		agent->setPath(path, true);

		float previousX = agent->getGlobalPosition().x;
		float minimumX = previousX;
		bool observedInteraction = false;
		float firstInteractionX = 0.0f;
		bool doubledBack = false;
		for (unsigned tick = 0; tick < 1000 && agent->getSector()->getIndex() == left; ++tick)
		{
			world.advanceTick();
			auto x = agent->getGlobalPosition().x;
			minimumX = std::min(minimumX, x);
			if (x > previousX + 0.0001f && minimumX < 0.75f) doubledBack = true;
			previousX = x;
			auto snapshot = world.getSimulationSnapshot();
			if (!observedInteraction && std::any_of(snapshot.interactionRequests.begin(),
				snapshot.interactionRequests.end(), [&](auto const& request)
					{ return request.actor == id; }))
			{
				observedInteraction = true;
				firstInteractionX = x;
			}
		}
		require(observedInteraction && firstInteractionX >= 0.85f,
			"Agent did not press the Shuttle call while passing it");
		require(!doubledBack, "Agent reached the Shuttle Door and doubled back to its call Button");
	}

	uint64_t cancelLiftJourney(unsigned boundary)
	{
		core::World world("Cancellation boundaries", 6, 4);
		auto lower = world.addCorridor(0, 0, 5);
		auto upper = world.addCorridor(2, 0, 5);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		auto lift = world.addLift(1, 0, 2, options);
		world.addSectorMarker(upper, 0, 4.5f, "Destination");
		world.finishBuild();
		auto id = world.createAgent("Passenger", lower, 0, 0.5f);
		auto agent = world.lookupAgent(id).entity;
		require(world.moveAgentToMarker(id, world.getMarkerIds()[0]).accepted(), "Lift movement refused");
		bool requested = false;
		uint64_t completed = 0;
		for (unsigned tick = 0; tick < 15000; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled)
				{
					require(!completed, "Duplicate cancellation completion");
					completed = event.tick;
				}
			if (completed)
			{
				auto snapshot = world.getSimulationSnapshot();
				require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Cancellation leaked traversal transactions");
				for (auto const& resource : snapshot.traversalResources)
					require(resource.occupantCount == 0 && resource.admissionReservationCount == 0, "Cancellation leaked capacity");
				for (auto const& request : snapshot.interactionRequests)
					require(request.actor != id || request.result != core::InteractionResult::Pending, "Cancellation leaked interaction");
				for (auto const& operation : snapshot.deviceOperations)
					require(std::find(operation.requesters.begin(), operation.requesters.end(), id) == operation.requesters.end(), "Cancellation leaked device-operation claim");
				require(agent->getSector()->getIndex() != lift.lift.sector->getIndex(), "Cancellation stranded passenger");
				world.advanceTicks(10);
				for (auto const& event : world.consumeSimulationEvents())
					require(event.type != core::SimulationEventType::MovementCancelled, "Repeated cancellation outcome");
				return completed;
			}
			bool queued = false, permitted = false, crossing = false, riding = false;
			auto snapshot = world.getSimulationSnapshot();
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == id && request.edgeType == core::EdgeType::Door)
				{
					queued = bool(request.queueTicket) && !request.permit;
					permitted = bool(request.permit);
					crossing = permitted && agent->getState() == core::Agent::State::TraversingEdge;
				}
			for (auto const& resource : snapshot.traversalResources)
				if (resource.id == lift.traversalResource)
					riding = resource.liftMoving && agent->getSector()->getIndex() == lift.lift.sector->getIndex();
			bool atBoundary = boundary == 0 ? queued : boundary == 1 ? permitted : boundary == 2 ? crossing : riding;
			if (!requested && atBoundary)
			{
				require(world.cancelAgentMovement(id).accepted(), "Boundary cancellation refused");
				requested = true;
			}
		}
		throw std::runtime_error("Lift cancellation did not complete");
	}
}

namespace routing_smoke
{
	void registerMovement(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "explicitIdleStableMarkerRequests", [](smoke::Context const&) { stableMarkerRequests(); } });
		checks.push_back({ "ordinaryCommands", [](smoke::Context const&)
		{
			ordinaryCommands();
		} });
		checks.push_back({ "cancelDoorCrossing", [](smoke::Context const&)
		{
			cancelDoorCrossing();
		} });
		checks.push_back({ "initialWaypointBeforeLiftCallIsSkipped", [](smoke::Context const&)
		{
			initialWaypointBeforeLiftCallIsSkipped();
		} });
		checks.push_back({ "liftCallIsPressedWhilePassing", [](smoke::Context const&)
		{
			liftCallIsPressedWhilePassing();
		} });
		checks.push_back({ "liftPassengerWalksToExitAlignment", [](smoke::Context const&)
		{
			liftPassengerWalksToExitAlignment();
		} });
		checks.push_back({ "shuttleCallIsPressedWhilePassing", [](smoke::Context const&)
		{
			shuttleCallIsPressedWhilePassing();
		} });
		checks.push_back({ "cancelLiftJourney0", [](smoke::Context const&)
		{
			require(cancelLiftJourney(0) == cancelLiftJourney(0), "Cancellation was not deterministic");
		} });
		checks.push_back({ "cancelLiftJourney1", [](smoke::Context const&)
		{
			require(cancelLiftJourney(1) == cancelLiftJourney(1), "Cancellation was not deterministic");
		} });
		checks.push_back({ "cancelLiftJourney2", [](smoke::Context const&)
		{
			require(cancelLiftJourney(2) == cancelLiftJourney(2), "Cancellation was not deterministic");
		} });
		checks.push_back({ "cancelLiftJourney3", [](smoke::Context const&)
		{
			require(cancelLiftJourney(3) == cancelLiftJourney(3), "Cancellation was not deterministic");
		} });
	}
}
