#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "core/Button.h"
#include "InteractionResults.h"

namespace
{
	using smoke::twoNodePath;
	constexpr uint64_t MaximumSimulationTicks = 1000;
	using namespace simulation_smoke;

	bool singleAgentDoorJourney(core::DoorActivationMode mode)
	{
		core::World world("Single-agent door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = mode;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 8.0f;
		auto created = world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& edge) { return edge->getType() == core::EdgeType::Door; });
		if (edgeIt == world.getGraph()->getEdges().end() || !created.traversalResource)
		{
			return false;
		}
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Door traveller", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);

		bool observedWaitingForFullOpen = false;
		bool observedVisibleCrossingLease = false;
		bool observedDoorOperationSucceeded = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks && agent->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto const& resource = snapshot.traversalResources.front();
			observedDoorOperationSucceeded = observedDoorOperationSucceeded || std::any_of(
				snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
				[](auto const& operation) { return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
			if (resource.doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()
				&& agent->getSector() == world.getSector(fore).get())
			{
				observedWaitingForFullOpen = true;
			}
			if (agent->getState() == core::Agent::State::TraversingEdge
				&& resource.doorState == core::DoorSnapshotState::Open
				&& resource.openLeaseCount == 1
				&& agent->getSector() == world.getSector(fore).get())
			{
				observedVisibleCrossingLease = true;
			}
		}

		auto completed = world.getSimulationSnapshot();
		if (!observedWaitingForFullOpen || !observedVisibleCrossingLease
			|| !observedDoorOperationSucceeded
			|| agent->getState() != core::Agent::State::Idle
			|| agent->getSector() != world.getSector(back).get()
			|| completed.traversalResources.front().doorActivationMode != mode
			|| completed.traversalResources.front().openLeaseCount != 0)
		{
			return false;
		}

		for (uint32_t i = 0; i < 240
			&& world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closed; ++i)
		{
			world.advanceTick();
		}
		return world.getSimulationSnapshot().traversalResources.front().doorState == core::DoorSnapshotState::Closed;
	}

	bool automaticBulkheadSensesNearbyNonTraveller()
	{
		auto observe = [](float sensorDistance, float agentX, bool fromRight = false)
		{
			core::World world("Automatic Bulkhead sensor", 8, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
			auto right = world.addRoom("Right", 0, 0, 3, 3, 1);
			core::World::CreateBulkheadDoorOptions options;
			options.activationMode = core::DoorActivationMode::Automatic;
			options.controls[0] = options.controls[1] = false;
			options.automaticSensorDistance = sensorDistance;
			world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT, options);
			world.finishBuild();

			// This Agent has no Path through the Bulkhead. Its physical presence is
			// the only possible source of automatic opening demand.
			world.createAgent("Nearby bystander", fromRight ? right : left, 0, agentX);
			world.advanceTick();
			return world.getSimulationSnapshot().traversalResources.front();
		};

		// The closed leaf spans x=2.9..3.1 and an Agent is 0.4 wide. An Agent
		// centred at x=2.4 has a 0.3 physical gap to the leaf: inside the 0.5
		// default, but outside a 0.25 per-instance override. Moving to x=2.5
		// reduces that gap to 0.2 and enters the overridden range.
		auto defaultRange = observe(CORE_BULKHEAD_DOOR_AUTOMATIC_SENSOR_DISTANCE, 2.4f);
		auto defaultRangeFromRight = observe(
			CORE_BULKHEAD_DOOR_AUTOMATIC_SENSOR_DISTANCE, 0.6f, true);
		auto outsideOverride = observe(0.25f, 2.4f);
		auto insideOverride = observe(0.25f, 2.5f);
		return defaultRange.doorState == core::DoorSnapshotState::Opening
			&& defaultRange.presenceObserved
			&& abs(defaultRange.automaticSensorDistance - 0.5f) < 0.0001f
			&& defaultRangeFromRight.doorState == core::DoorSnapshotState::Opening
			&& defaultRangeFromRight.presenceObserved
			&& outsideOverride.doorState == core::DoorSnapshotState::Closed
			&& !outsideOverride.presenceObserved
			&& insideOverride.doorState == core::DoorSnapshotState::Opening
			&& insideOverride.presenceObserved;
	}

	bool bulkheadAndWindowThresholdsUseTraversalResources()
	{
		// A closed bulkhead still coordinates opening, but once fully open it is an
		// unconstrained bidirectional passage: all waiting Agents can cross without
		// queue or crossing-lane serialization.
		core::World bulkheadWorld("Bulkhead threshold", 8, 2);
		auto left = bulkheadWorld.addRoom("Left", 0, 0, 0, 3, 1);
		auto right = bulkheadWorld.addRoom("Right", 0, 0, 3, 3, 1);
		core::World::CreateBulkheadDoorOptions bulkheadOptions;
		bulkheadOptions.activationMode = core::DoorActivationMode::Manual;
		bulkheadOptions.controls[0] = bulkheadOptions.controls[1] = false;
		auto bulkhead = bulkheadWorld.addSectorBulkheadDoor(0, 0, 3,
			CORE_SIDE_LEFT, bulkheadOptions);
		bulkheadWorld.finishBuild();
		auto bulkheadEdge = std::find_if(bulkheadWorld.getGraph()->getEdges().begin(),
			bulkheadWorld.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::BulkheadDoor; });
		if (bulkheadEdge == bulkheadWorld.getGraph()->getEdges().end()
			|| (*bulkheadEdge)->getTraversalResourceId() != bulkhead.traversalResource) return false;
		auto source = (*bulkheadEdge)->getVertex(0)->getSector()->getIndex() == left
			? (*bulkheadEdge)->getVertex(0) : (*bulkheadEdge)->getVertex(1);
		auto destination = (*bulkheadEdge)->getOtherVertex(source);
		auto agentId = bulkheadWorld.createAgent("Left bulkhead traveller", left, 0, 1.0f);
		auto opposingId = bulkheadWorld.createAgent("Right bulkhead traveller", right, 0, 1.0f);
		auto agent = bulkheadWorld.lookupAgent(agentId).entity;
		auto opposing = bulkheadWorld.lookupAgent(opposingId).entity;
		agent->setPath(twoNodePath(source, destination, *bulkheadEdge), true);
		opposing->setPath(twoNodePath(destination, source, *bulkheadEdge), true);
		bool waitedForOpen = false;
		bool observedOpenBidirectionalPassage = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& (agent->getState() != core::Agent::State::Idle
				|| opposing->getState() != core::Agent::State::Idle); ++i)
		{
			bulkheadWorld.advanceTick();
			auto const& snapshot = bulkheadWorld.getSimulationSnapshot();
			if (!snapshot.traversalResources.empty()
				&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()) waitedForOpen = true;
			if (!snapshot.traversalResources.empty()
				&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Open
				&& snapshot.traversalPermits.size() == 2
				&& agent->getState() == core::Agent::State::TraversingEdge
				&& opposing->getState() == core::Agent::State::TraversingEdge
				&& std::all_of(snapshot.traversalResources.front().queueLanes.begin(),
					snapshot.traversalResources.front().queueLanes.end(), [](auto const& lane)
					{ return lane.queue.empty(); })
				&& std::all_of(snapshot.traversalResources.front().crossingLanes.begin(),
					snapshot.traversalResources.front().crossingLanes.end(), [](auto const& lane)
					{ return !lane.owner; }))
			{
				observedOpenBidirectionalPassage = true;
			}
		}
		if (!waitedForOpen || !observedOpenBidirectionalPassage
			|| agent->getSector() != bulkheadWorld.getSector(right).get()
			|| opposing->getSector() != bulkheadWorld.getSector(left).get()
			|| !bulkheadWorld.getSimulationSnapshot().traversalRequests.empty()) return false;

		// Traversable windows contribute conditional topology, and only the clear,
		// fully-open state can receive a permit.
		core::World windowWorld("Window threshold", 6, 2);
		auto fore = windowWorld.addRoom("Fore", 0, 0, 0, 5, 1);
		auto back = windowWorld.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateWindowOptions windowOptions;
		windowOptions.traversable = true;
		windowOptions.initialState = core::Window::State::Open;
		auto window = windowWorld.addSectorWindow(0, 0, 2, 1, 1, windowOptions);
		windowWorld.finishBuild();
		auto windowEdge = std::find_if(windowWorld.getGraph()->getEdges().begin(),
			windowWorld.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::Window; });
		if (windowEdge == windowWorld.getGraph()->getEdges().end()
			|| (*windowEdge)->getTraversalResourceId() != window.traversalResource
			|| !window.object->isNormallyTraversable()) return false;
		constexpr core::Window::State blockedStates[] = {
			core::Window::State::Closed, core::Window::State::Opening,
			core::Window::State::Closing, core::Window::State::Broken,
			core::Window::State::Frosted, core::Window::State::Frosting,
			core::Window::State::Unfrosting, core::Window::State::Tinted,
			core::Window::State::Tinting, core::Window::State::Untinting
		};
		for (auto state : blockedStates)
		{
			window.object->setState(state);
			if ((*windowEdge)->isTraversable({}, {})) return false;
		}
		window.object->setState(core::Window::State::Open, core::Window::Style::Tinted);
		if ((*windowEdge)->isTraversable({}, {})) return false;
		window.object->setState(core::Window::State::Open, core::Window::Style::Frosted);
		if ((*windowEdge)->isTraversable({}, {})) return false;
		window.object->setState(core::Window::State::Open);
		auto windowSource = (*windowEdge)->getVertex(0)->getSector()->getIndex() == fore
			? (*windowEdge)->getVertex(0) : (*windowEdge)->getVertex(1);
		auto windowDestination = (*windowEdge)->getOtherVertex(windowSource);
		auto windowAgentId = windowWorld.createAgent("Window traveller", fore, 0, 0.5f);
		auto windowAgent = windowWorld.lookupAgent(windowAgentId).entity;
		windowAgent->setPath(twoNodePath(windowSource, windowDestination, *windowEdge), true);
		for (uint32_t i = 0; i < MaximumSimulationTicks && windowAgent->getState() != core::Agent::State::Idle; ++i)
			windowWorld.advanceTick();
		return windowAgent->getSector() == windowWorld.getSector(back).get()
			&& windowWorld.getSimulationSnapshot().traversalRequests.empty();
	}

	bool agentsPressUpcomingDoorButtonsWhilePassing()
	{
		// Reproduce the Citadel route: the Button's Interactable vertex is part of
		// the in-sector path leading from the far Door to the controlled Door.
		core::World world("Opportunistic remote door", 8, 2);
		auto corridor = world.addCorridor(0, 1, 5);
		world.addRoom("Destination", 1, 0, 0, 3, 1);
		world.addRoom("Far room", 1, 0, 4, 3, 1);
		uint32_t markerId;
		world.addSectorMarker(1, 0, 0.5f, &markerId);
		core::World::CreateDoorOptions remote;
		remote.activationMode = core::DoorActivationMode::RemoteControlled;
		remote.controls[0] = true;
		auto created = world.addSectorDoor(0, 0, 1, remote);
		world.addSectorDoor(0, 0, 5);
		world.finishBuild();

		auto target = world.getGraph()->getVertexByIdentifier(markerId);
		std::vector<core::AgentId> ids = {
			world.createAgent("Early presser one", corridor, 0, 2.75f),
			world.createAgent("Early presser two", corridor, 0, 2.75f)
		};
		for (auto id : ids)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path || path->nodes.size() < 3) return false;
			bool reachesButtonBeforeDoor = false;
			for (auto const& node : path->nodes)
			{
				if (node.edge && node.edge->getType() == core::EdgeType::Door) break;
				reachesButtonBeforeDoor = reachesButtonBeforeDoor || (node.targetVertex
					&& node.targetVertex->getSubType() == core::VertexSubType::Interactable);
			}
			if (!reachesButtonBeforeDoor) return false;
			agent->setPath(std::move(path), true);
		}

		bool observedIndependentPressesBeforeDoorRequest = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			bool hasDoorRequest = std::any_of(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(), [](auto const& request)
					{ return request.edgeType == core::EdgeType::Door; });
			if (!hasDoorRequest && snapshot.interactionRequests.size() == 2
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2)
			{
				observedIndependentPressesBeforeDoorRequest = true;
			}
			if (std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return observedIndependentPressesBeforeDoorRequest
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
			{
				return world.lookupAgent(id).entity->getSector() == world.getSector(1).get();
			})
			&& world.lookupInteractionPoint(created.controls[0].interactionPoint).entity->getReach()
				== core::Agent::physicalBaselineForType("Human").standingHeight * 0.4f;
	}

	bool remoteDoorUsesOnePhysicalOperatorAndSharedOperation()
	{
		core::World world("Shared remote door", 7, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 6, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = true;
		options.controls[1] = true;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();

		for (auto const& control : created.controls)
		{
			auto object = control.sector->getObject(control.index)->_getObject();
			auto button = std::dynamic_pointer_cast<core::Button>(object);
			if (!button || !control.interactionPoint
				|| button->getInteractionPointId() != control.interactionPoint
				|| !world.lookupInteractionPoint(control.interactionPoint)) return false;
		}

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = world.createAgent("First remote waiter", fore, 0, 0.4f);
		auto secondId = world.createAgent("Second remote waiter", fore, 0, 0.6f);
		auto first = world.lookupAgent(firstId).entity;
		auto second = world.lookupAgent(secondId).entity;
		first->setPath(twoNodePath(source, destination, edge), true);
		second->setPath(twoNodePath(source, destination, edge), true);

		bool observedSharedPreparation = false;
		bool cancelledFirst = false;
		bool observedSharedOperationSucceeded = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& second->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			observedSharedOperationSucceeded = observedSharedOperationSucceeded || std::any_of(
				snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
				[](auto const& operation) { return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
			if (!cancelledFirst && snapshot.traversalRequests.size() == 2
				&& snapshot.interactionRequests.size() == 1
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2
				&& snapshot.traversalResources.front().preparationOperator
				&& snapshot.traversalResources.front().activePreparation)
			{
				observedSharedPreparation = true;
				first->clearPath();
				cancelledFirst = true;
			}
		}

		return observedSharedPreparation && cancelledFirst
			&& observedSharedOperationSucceeded
			&& first->getSector() == world.getSector(fore).get()
			&& second->getState() == core::Agent::State::Idle
			&& second->getSector() == world.getSector(back).get();
	}

	bool remoteDoorWithoutReachableControlIsUnavailable()
	{
		core::World world("Uncontrolled remote door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = false;
		options.controls[1] = false;
		auto created = world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Stranded remote waiter", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(400);
		auto snapshot = world.getSimulationSnapshot();
		return snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.traversalRequests.front().failureReason == core::TraversalFailureReason::NoReachableControl
			&& snapshot.deviceOperations.empty() && snapshot.interactionRequests.empty();
	}

	bool wideDoorLanesAndGracefulDisableAreSafe()
	{
		core::World world("Wide safe door", 9, 2);
		auto fore = world.addRoom("Wide fore", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Wide back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options;
		options.width = 2;
		options.crossingLanes = 2;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Wide first", fore, 0, 4.0f),
			world.createAgent("Wide second", fore, 0, 4.0f),
			world.createAgent("Disabled waiter", fore, 0, 4.0f)
		};
		for (auto id : ids) world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		bool disabledWithTwoCrossings = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 2 || snapshot.traversalResources.front().crossingLanes.size() != 2)
				return false;
			if (!disabledWithTwoCrossings && snapshot.traversalPermits.size() == 2)
			{
				auto const& lanes = snapshot.traversalResources.front().crossingLanes;
				if (!lanes[0].owner || !lanes[1].owner || lanes[0].owner == lanes[1].owner
					|| snapshot.traversalResources.front().crossingLeaseCount != 2)
					return false;
				disabledWithTwoCrossings = world.setTraversalResourceEnabled(created.traversalResource, false);
			}
			if (disabledWithTwoCrossings
				&& world.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}
		world.advanceTicks(3);
		auto snapshot = world.getSimulationSnapshot();
		auto denied = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		return disabledWithTwoCrossings
			&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(ids[1]).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(ids[2]).entity->getSector() == world.getSector(fore).get()
			&& denied != snapshot.traversalRequests.end()
			&& denied->state == core::TraversalRequestState::Denied
			&& denied->failureReason == core::TraversalFailureReason::ResourceDisabled
			&& snapshot.traversalResources.front().crossingLeaseCount == 0;
	}

	bool doorLeasesAndSensorObservationsPreventUnsafeClosure()
	{
		core::World world("Door observation safety", 7, 2);
		auto fore = world.addRoom("Sensor fore", 0, 0, 0, 6, 1);
		world.addRoom("Sensor back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 2.0f;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto sensor = core::DoorSensorId{ 1 };
		if (!world.setDoorSensorObservation(created.traversalResource, sensor,
			core::DoorSensorObservation::Presence)) return false;
		world.advanceTicks(150);
		auto lease = world.acquireDoorOpenLease(created.traversalResource);
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		world.advanceTicks(30);
		auto snapshot = world.getSimulationSnapshot();
		if (!lease || snapshot.traversalResources.front().doorState != core::DoorSnapshotState::Open
			|| snapshot.traversalResources.front().externalOpenLeaseCount != 1) return false;

		auto actor = world.createAgent("Close operator", fore, 0, 3.5f);
		core::DeviceCommand close;
		close.type = core::DeviceCommandType::OpenDoor;
		close.desiredState = false;
		close.traversalResource = created.traversalResource;
		auto point = world.createInteractionPoint("Close door", core::SectorId{ (uint64_t)fore + 1 },
			{ 3.5f, 0.0f }, 1.0f, 0.0f, { { close, core::InteractionBindingRequirement::Required } });
		auto closeRequest = world.requestInteraction(point, actor);
		world.advanceTicks(4);
		if (observedInteractionResult(world, closeRequest) != core::InteractionResult::Rejected
			|| !world.releaseDoorOpenLease(created.traversalResource, lease)) return false;

		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		world.advanceTicks(20);
		if (world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Open) return false;
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		for (uint32_t i = 0; i < 10 && world.getSimulationSnapshot().traversalResources.front().doorState
			!= core::DoorSnapshotState::Closing; ++i) world.advanceTick();
		if (world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closing) return false;
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		world.advanceTick();
		snapshot = world.getSimulationSnapshot();
		return snapshot.traversalResources.front().obstructionObserved
			&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening;
	}

	bool unavailableDoorRejectsTraversal()
	{
		core::World world("Unavailable door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Unavailable;
		world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Rejected traveller", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		// This is the same two-step path assignment used by the UI for a
		// player-directed agent: preview the route, then explicitly start it.
		agent->setPath(twoNodePath(source, destination, edge), false);
		world.advanceTick();
		if (agent->getState() != core::Agent::State::Idle
			|| !world.getSimulationSnapshot().traversalRequests.empty()) return false;
		agent->startPathing();
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& world.getSimulationSnapshot().traversalRequests.empty(); ++i)
		{
			world.advanceTick();
		}
		auto snapshot = world.getSimulationSnapshot();
		return agent->getSector() == world.getSector(fore).get()
			&& agent->getState() == core::Agent::State::WaitingForTraversal
			&& snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.deviceOperations.empty() && snapshot.traversalPermits.empty();
	}
}

void registerDoors(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "singleAgentDoorJourneyManual", [](smoke::Context const&)
		{
			smoke::require(singleAgentDoorJourney(core::DoorActivationMode::Manual), "manual door journey or hold-open safety failed");
		} });
	checks.push_back({ "singleAgentDoorJourneyAutomatic", [](smoke::Context const&)
		{
			smoke::require(singleAgentDoorJourney(core::DoorActivationMode::Automatic), "automatic door journey or hold-open safety failed");
		} });
	checks.push_back({ "automaticBulkheadSensesNearbyNonTraveller", [](smoke::Context const&)
		{
			smoke::require(automaticBulkheadSensesNearbyNonTraveller(), "automatic Bulkhead Door did not sense a nearby non-travelling Agent");
		} });
	checks.push_back({ "bulkheadAndWindowThresholdsUseTraversalResources", [](smoke::Context const&)
		{
			smoke::require(bulkheadAndWindowThresholdsUseTraversalResources(), "bulkhead or window threshold migration failed");
		} });
	checks.push_back({ "agentsPressUpcomingDoorButtonsWhilePassing", [](smoke::Context const&)
		{
			smoke::require(agentsPressUpcomingDoorButtonsWhilePassing(), "agents did not press upcoming Door Buttons while passing");
		} });
	checks.push_back({ "remoteDoorUsesOnePhysicalOperatorAndSharedOperation", [](smoke::Context const&)
		{
			smoke::require(remoteDoorUsesOnePhysicalOperatorAndSharedOperation(), "remote door did not share physical preparation or survive operator cancellation");
		} });
	checks.push_back({ "remoteDoorWithoutReachableControlIsUnavailable", [](smoke::Context const&)
		{
			smoke::require(remoteDoorWithoutReachableControlIsUnavailable(), "remote door without a reachable control was not reported unavailable");
		} });
	checks.push_back({ "wideDoorLanesAndGracefulDisableAreSafe", [](smoke::Context const&)
		{
			smoke::require(wideDoorLanesAndGracefulDisableAreSafe(), "wide door lanes exceeded capacity or deactivation was unsafe");
		} });
	checks.push_back({ "doorLeasesAndSensorObservationsPreventUnsafeClosure", [](smoke::Context const&)
		{
			smoke::require(doorLeasesAndSensorObservationsPreventUnsafeClosure(), "door leases or sensor observations allowed unsafe closure");
		} });
	checks.push_back({ "unavailableDoorRejectsTraversal", [](smoke::Context const&)
		{
			smoke::require(unavailableDoorRejectsTraversal(), "unavailable door did not reject traversal");
		} });
}
