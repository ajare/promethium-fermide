#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/Staircase.h"
#include "core/Transit.h"

namespace
{
	bool singlePassengerCompletesTwoStopLiftJourney()
	{
		core::World world("Two-stop lift journey", 6, 4);
		auto lower = world.addCorridor(0, 0, 5);
		auto upper = world.addCorridor(2, 0, 5);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();
		if (!created.traversalResource || created.doors.size() != 2 || !created.interiorSelector)
			return false;
		auto initial = world.getSimulationSnapshot();
		auto initialLift = std::find_if(initial.traversalResources.begin(),
			initial.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		if (initialLift == initial.traversalResources.end() || initialLift->capacity != 2) return false;
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 2.5f, 2.0f });
		auto passengerId = world.createAgent("Lift passenger", lower, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		uint32_t boardingEdges = 0, rideEdges = 0;
		for (auto const& node : path->nodes)
		{
			if (!node.edge) continue;
			boardingEdges += node.edge->getType() == core::EdgeType::Door;
			rideEdges += node.edge->getType() == core::EdgeType::Lift;
		}
		if (boardingEdges != 2 || rideEdges != 1) return false;
		passenger->setPath(path, true);

		bool sawIntentWithoutDispatch = false;
		bool sawReservedCapacity = false;
		bool sawOnboard = false;
		bool enteredAtWalkSpeed = false;
		bool enteredBeforeAssignedPosition = false;
		bool walkedToAssignedPosition = false;
		bool sawConfirmedDestination = false;
		bool sawMovingAttachedPassenger = false;
		bool sawQueuedDebug = false, sawEnteringDebug = false;
		bool sawInLiftDebug = false, sawExitingDebug = false;
		bool climbedTowardLandingCallButton = false;
		bool wasOnboard = false;
		auto previousPosition = passenger->getGlobalPosition();
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4
			&& passenger->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			if (passenger->getSector() == world.getSector(lower).get()
				&& passenger->getGlobalPosition().y > 0.001f)
				climbedTowardLandingCallButton = true;
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end() || !lift->isLift) return false;
			for (auto const& debug : lift->liftAgents)
			{
				if (debug.agent != passengerId || debug.targetStop != 1
					|| std::abs(debug.targetLevel - 2.0f) > 0.001f) continue;
				sawQueuedDebug = sawQueuedDebug
					|| debug.state == core::LiftAgentState::QueuingAtDoor;
				sawEnteringDebug = sawEnteringDebug
					|| debug.state == core::LiftAgentState::Entering;
				sawInLiftDebug = sawInLiftDebug
					|| debug.state == core::LiftAgentState::InLift;
				sawExitingDebug = sawExitingDebug
					|| debug.state == core::LiftAgentState::Exiting;
			}
			if (!snapshot.traversalRequests.empty() && !lift->liftPassenger
				&& std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
					[](auto const& operation)
					{
						return operation.command.type == core::DeviceCommandType::CallLift
							&& operation.state == core::DeviceOperationState::Pending;
					}))
				sawIntentWithoutDispatch = true;
			sawReservedCapacity = sawReservedCapacity || lift->admissionReservationCount == 1;
			auto const onboard = lift->liftPassenger == passengerId
				&& passenger->getSector() == world.getSector(created.lift.sector->getIndex()).get();
			sawOnboard = sawOnboard || onboard;
			auto assigned = std::find_if(lift->capacityPositions.begin(), lift->capacityPositions.end(),
				[&](auto const& position) { return position.occupant == passengerId; });
			if (onboard && assigned != lift->capacityPositions.end())
			{
				auto const assignedX = world.getSector(created.lift.sector->getIndex())->getPosition().x
					+ assigned->position.x;
				if (!wasOnboard)
				{
					enteredAtWalkSpeed = passenger->getGlobalPosition().distanceTo(previousPosition)
						<= passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
					enteredBeforeAssignedPosition = std::abs(
						passenger->getGlobalPosition().x - assignedX) > 0.01f;
				}
				else if (std::abs(passenger->getGlobalPosition().x - previousPosition.x) > 0.0001f)
				{
					if (passenger->getGlobalPosition().distanceTo(previousPosition)
						> passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f) return false;
					walkedToAssignedPosition = walkedToAssignedPosition
						|| std::abs(passenger->getGlobalPosition().x - assignedX) < 0.01f;
				}
			}
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SelectLiftDestination
					&& operation.state == core::DeviceOperationState::Succeeded)
					sawConfirmedDestination = true;
			if (lift->liftMoving)
			{
				for (auto const& resource : snapshot.traversalResources)
					if (resource.isDoor && (!resource.crossingLanes.empty()
						&& (resource.crossingOwner || resource.doorState != core::DoorSnapshotState::Closed)))
						return false;
				if (std::abs(passenger->getGlobalPosition().y - lift->liftPosition) < 0.001f)
					sawMovingAttachedPassenger = true;
			}
			wasOnboard = onboard;
			previousPosition = passenger->getGlobalPosition();
		}
		auto final = world.getSimulationSnapshot();
		auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawIntentWithoutDispatch && sawReservedCapacity && sawOnboard
			&& enteredAtWalkSpeed && enteredBeforeAssignedPosition && walkedToAssignedPosition
			&& sawConfirmedDestination && sawMovingAttachedPassenger
			&& sawQueuedDebug && sawEnteringDebug && sawInLiftDebug && sawExitingDebug
			&& !climbedTowardLandingCallButton
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(upper).get()
			&& lift != final.traversalResources.end() && !lift->liftPassenger
			&& lift->occupantCount == 0;
	}

	bool liftDoorQueueRequestsBeforeOccupiedTail()
	{
		core::World world("Early Lift Door queue", 8, 4);
		auto lower = world.addCorridor(0, 0, 7);
		auto upper = world.addCorridor(2, 0, 7);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 1;
		auto created = world.addLift(1, 0, 3, options);
		uint32_t approachId;
		world.addSectorMarker(lower, 0, 2.75f, &approachId);
		world.finishBuild();

		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 3.5f, 2.0f });
		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 3.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;

		// Send the sole-capacity car away with an occupant so the lower landing
		// queue remains unavailable while the following Agents approach it.
		auto rider = world.createAgent("Descending rider", upper, 0, 3.5f);
		auto riderEntity = world.lookupAgent(rider).entity;
		auto riderPath = world.getGraph()->calculatePath(riderEntity, lowerTarget);
		if (!riderPath) return false;
		riderEntity->setPath(std::move(riderPath), true);
		bool descending = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (lift != snapshot.traversalResources.end() && lift->liftMoving
				&& lift->liftDirection == core::TraversalDirection::Descending
				&& lift->occupantCount == 1)
			{
				descending = true;
				break;
			}
		}
		if (!descending) return false;

		auto blocker = world.createAgent("Lift queue head", lower, 0, 3.5f);
		auto blockerEntity = world.lookupAgent(blocker).entity;
		auto blockerPath = world.getGraph()->calculatePath(blockerEntity, upperTarget);
		if (!blockerPath) return false;
		blockerEntity->setPath(std::move(blockerPath), true);
		bool queueEstablished = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto landing = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.doors.front().traversalResource; });
			if (landing != snapshot.traversalResources.end()
				&& any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request) { return request.resource == created.doors.front().traversalResource
						&& request.state == core::TraversalRequestState::Pending
						&& request.hasQueuePosition; }))
			{
				queueEstablished = true;
				break;
			}
		}
		if (!queueEstablished) return false;

		auto approach = world.getGraph()->getVertexByIdentifier(approachId);
		auto waiter = world.createAgent("Lift waiter", lower, 0, 2.75f);
		auto waiterEntity = world.lookupAgent(waiter).entity;
		auto path = world.getGraph()->calculatePath(waiterEntity, approach, upperTarget);
		if (!path) return false;
		waiterEntity->setPath(std::move(path), true);
		bool requestedBeforeOccupiedTail = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			auto const positionBeforeTick = waiterEntity->getGlobalPosition();
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiter
					&& value.resource == created.doors.front().traversalResource
					&& value.state == core::TraversalRequestState::Pending; });
			if (request == snapshot.traversalRequests.end()) continue;
			auto landing = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.doors.front().traversalResource; });
			if (landing == snapshot.traversalResources.end()) return false;
			if (request->queueApproach >= landing->queueLanes.size()) continue;
			auto const& lane = landing->queueLanes[request->queueApproach];
			requestedBeforeOccupiedTail = requestedBeforeOccupiedTail
				|| positionBeforeTick.x < lane.origin.x;
			if (!request->hasQueuePosition) continue;
			auto const target = lane.positions[request->queuePosition].position;
			return requestedBeforeOccupiedTail && target.x <= lane.origin.x + 0.001f;
		}
		return false;
	}

	bool liftCallOperatorDoesNotFightItsQueuePosition()
	{
		core::World world("Lift call operator queue", 16, 3);
		auto bottom = world.addCorridor(0, 0, 16);
		auto middle = world.addCorridor(1, 0, 16);
		auto top = world.addCorridor(2, 0, 16);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.75f;
		options.maximumBoardingSeconds = 5.0f;
		world.addLift(1, 0, 8, options);
		uint32_t bottomTargetId, middleTargetId, topTargetId;
		world.addSectorMarker(bottom, 0, 0.5f, &bottomTargetId);
		world.addSectorMarker(middle, 0, 0.5f, &middleTargetId);
		world.addSectorMarker(top, 0, 0.5f, &topTargetId);
		world.finishBuild();

		auto graph = world.getGraph();
		auto bottomTarget = graph->getVertexByIdentifier(bottomTargetId);
		auto middleTarget = graph->getVertexByIdentifier(middleTargetId);
		auto topTarget = graph->getVertexByIdentifier(topTargetId);
		struct Group { uint32_t sector; std::shared_ptr<const core::Vertex> target; char const* name; };
		Group groups[] = {
			{ bottom, topTarget, "Bottom Right" },
			{ middle, bottomTarget, "Middle Right" },
			{ top, middleTarget, "Top Right" }
		};
		std::vector<core::AgentId> agents;
		for (auto const& group : groups)
			for (uint32_t i = 0; i < 3; ++i)
			{
				auto id = world.createAgent(
					std::string(group.name) + " " + std::to_string(i + 1),
					group.sector, 0, 14.75f - i * 0.75f);
				auto agent = world.lookupAgent(id).entity;
				auto path = graph->calculatePath(agent, group.target);
				if (!path) return false;
				agent->setPath(std::move(path), true);
				agents.push_back(id);
			}

		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 12; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[](auto const& request)
				{ return request.failureReason == core::TraversalFailureReason::LocalGoalUnreachable; }))
				return false;
			if (std::all_of(agents.begin(), agents.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; }))
				return world.lookupAgent(agents[4]).entity->getSector()
					== world.getSector(bottom).get();
		}
		return false;
	}

	bool waitingLiftPassengersFillArrivingCar()
	{
		core::World world("Arriving lift boards waiting capacity", 7, 4);
		auto lower = world.addCorridor(0, 0, 6);
		auto upper = world.addCorridor(2, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();

		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 2.5f, 0.0f });
		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 2.5f, 2.0f });
		if (!lowerTarget || !upperTarget) return false;
		auto downId = world.createAgent("Down passenger", upper, 0, 2.0f);
		auto down = world.lookupAgent(downId).entity;
		auto downPath = world.getGraph()->calculatePath(down, lowerTarget);
		if (!downPath) return false;
		down->setPath(downPath, true);

		bool descending = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift != snapshot.traversalResources.end() && lift->liftMoving
				&& lift->liftDirection == core::TraversalDirection::Descending
				&& lift->occupantCount == 1)
			{ descending = true; break; }
		}
		if (!descending) return false;

		for (uint32_t i = 0; i < 2; ++i)
		{
			auto id = world.createAgent("Waiting passenger", lower, 0, 1.7f - i * 0.35f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		bool sawBothWaiting = false;
		bool sawHeldWaitersWhileDisembarking = false;
		std::map<core::AgentId, float> previousDisembarkTargets;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 5; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()) return false;
			auto waiting = std::count_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request)
				{ return request.resource == created.doors.front().traversalResource
					&& request.state == core::TraversalRequestState::Pending; });
			sawBothWaiting = sawBothWaiting || waiting == 2;
			if (lift->liftStopPhase == core::LiftStopPhase::Disembarking
				&& lift->liftCurrentStop == 0)
			{
				uint32_t held = 0;
				for (auto const& request : snapshot.traversalRequests)
				{
					if (request.sourceSector.value != lower + 1 || !request.queueTicket
						|| request.state != core::TraversalRequestState::Pending
						|| !request.hasQueueStandingTarget) continue;
					++held;
					auto const distance = std::abs(
						request.queueStandingTarget.x - request.sourceEndpoint.x);
					if (auto previous = previousDisembarkTargets.find(request.owner);
						previous != previousDisembarkTargets.end()
						&& distance + 0.001f < previous->second) return false;
					previousDisembarkTargets[request.owner] = distance;
					if (request.permit || request.hasCapacityPosition) return false;
				}
				sawHeldWaitersWhileDisembarking = sawHeldWaitersWhileDisembarking || held >= 2;
			}
			if (lift->liftMoving && lift->liftDirection == core::TraversalDirection::Ascending
				&& lift->liftCurrentStop == 0)
				return sawBothWaiting && sawHeldWaitersWhileDisembarking
					&& lift->occupantCount == options.capacity;
		}
		return false;
	}

	bool liftCapacityAndStopPhasesAreEnforced()
	{
		core::World world("Finite lift", 7, 4);
		auto lower = world.addCorridor(0, 0, 6);
		auto upper = world.addCorridor(2, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();
		auto initial = world.getSimulationSnapshot();
		for (auto const& door : created.doors)
		{
			auto resource = std::find_if(initial.traversalResources.begin(),
				initial.traversalResources.end(),
				[&](auto const& candidate) { return candidate.id == door.traversalResource; });
			if (resource == initial.traversalResources.end()) return false;
			bool foundCarLane = false, foundCorridorLane = false;
			for (auto const& lane : resource->queueLanes)
			{
				auto sector = world.getSector((uint32_t)lane.sector.value - 1);
				if (sector->getIndex() == created.lift.sector->getIndex())
				{
					foundCarLane = true;
					auto landingY = door.door.sector->getObject(door.door.index)->getCellY();
					if (lane.positions.size() != options.capacity
						|| std::any_of(lane.positions.begin(), lane.positions.end(),
							[landingY](auto const& position)
							{ return std::abs(position.position.y - landingY) > 0.001f; })) return false;
				}
				else
				{
					foundCorridorLane = true;
					if (lane.positions.size() <= options.capacity) return false;
				}
			}
			if (!foundCarLane || !foundCorridorLane) return false;
		}
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 2.5f, 2.0f });
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = world.createAgent("Capacity passenger", lower, 0, 0.3f + i * 0.15f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			passengers.push_back(id);
		}

		bool sawFullCarWithWaitingPassenger = false;
		bool sawCutoffHonorReservations = false;
		bool sawDistinctCorridorQueuePositions = false;
		bool checkedFirstDepartureCapacity = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			std::vector<core::Vector2> corridorQueueTargets;
			for (auto const& request : snapshot.traversalRequests)
			{
				if (request.resource != created.doors.front().traversalResource) continue;
				// Admitted boarders release their waiting position to approach the
				// threshold, but retain their logical ticket and capacity reservation.
				if (request.state == core::TraversalRequestState::Pending
					&& request.hasCapacityPosition && !request.queueTicket) return false;
				if (request.hasQueuePosition)
					corridorQueueTargets.push_back(request.queuePositionTarget);
			}
			if (corridorQueueTargets.size() >= 2)
			{
				for (size_t i = 0; i < corridorQueueTargets.size(); ++i)
					for (size_t j = i + 1; j < corridorQueueTargets.size(); ++j)
						if (corridorQueueTargets[i].distanceTo(corridorQueueTargets[j])
							< CORE_RESOURCE_QUEUE_SLOT_PITCH - 0.001f) return false;
				sawDistinctCorridorQueuePositions = true;
			}
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()
				|| lift->occupantCount + lift->admissionReservationCount > options.capacity)
				return false;
			if (!checkedFirstDepartureCapacity && lift->liftMoving
				&& lift->liftCurrentStop == 0
				&& lift->liftDirection == core::TraversalDirection::Ascending)
			{
				checkedFirstDepartureCapacity = true;
				if (lift->occupantCount != options.capacity) return false;
			}
			if (lift->occupantCount == options.capacity && !lift->admissionQueue.empty())
				sawFullCarWithWaitingPassenger = true;
			if (snapshot.tick > lift->liftBoardingCutoffTick && lift->admissionReservationCount > 0)
				sawCutoffHonorReservations = true;
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{
					auto agent = world.lookupAgent(id).entity;
					return agent && agent->getState() == core::Agent::State::Idle
						&& agent->getSector() == world.getSector(upper).get();
				}))
			{
				return sawFullCarWithWaitingPassenger && sawCutoffHonorReservations
					&& sawDistinctCorridorQueuePositions && checkedFirstDepartureCapacity;
			}
		}
		return false;
	}

	bool multiStopLiftUsesDeterministicLookScheduling()
	{
		core::World world("LOOK lift", 7, 7);
		auto lower = world.addCorridor(0, 0, 6);
		auto middle = world.addCorridor(2, 0, 6);
		auto upper = world.addCorridor(5, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2, 5 }; // deliberately non-uniform
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 3.0f;
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();

		auto graph = world.getGraph();
		auto lowerTarget = graph->getClosestVertexInSector(world.getSector(lower).get(), { 2.5f, 0.0f });
		auto middleTarget = graph->getClosestVertexInSector(world.getSector(middle).get(), { 2.5f, 2.0f });
		auto upperTarget = graph->getClosestVertexInSector(world.getSector(upper).get(), { 2.5f, 5.0f });
		if (!lowerTarget || !middleTarget || !upperTarget) return false;

		struct Journey { core::AgentId id; std::shared_ptr<const core::Vertex> target; };
		std::vector<Journey> journeys = {
			{ world.createAgent("Up through run", lower, 0, 2.5f), upperTarget },
			{ world.createAgent("Down middle", middle, 0, 2.5f), lowerTarget },
			{ world.createAgent("Down upper", upper, 0, 2.5f), middleTarget }
		};
		for (auto const& journey : journeys)
		{
			auto agent = world.lookupAgent(journey.id).entity;
			auto path = graph->calculatePath(agent, journey.target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		std::vector<uint32_t> serviceOrder;
		core::LiftStopPhase previousPhase = core::LiftStopPhase::Idle;
		float previousPosition = 0.0f;
		bool observedCoalescedMiddleDemand = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 12; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()) return false;
			if (lift->liftStopRequestOwnerCounts.size() != 3
				|| lift->liftStopOldestRequestTicks.size() != 3) return false;
			observedCoalescedMiddleDemand = observedCoalescedMiddleDemand
				|| lift->liftStopRequestOwnerCounts[1] >= 2;
			if (lift->liftStopPhase == core::LiftStopPhase::Opening
				&& previousPhase != core::LiftStopPhase::Opening)
				serviceOrder.push_back(lift->liftCurrentStop);
			if (lift->liftMoving)
			{
				if (lift->liftDirection == core::TraversalDirection::Ascending
					&& lift->liftPosition + 0.0001f < previousPosition) return false;
				if (lift->liftDirection == core::TraversalDirection::Descending
					&& lift->liftPosition > previousPosition + 0.0001f) return false;
			}
			previousPhase = lift->liftStopPhase;
			previousPosition = lift->liftPosition;
			if (std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
				{ return world.lookupAgent(journey.id).entity->getState() == core::Agent::State::Idle; }))
				break;
		}

		return observedCoalescedMiddleDemand
			&& serviceOrder == std::vector<uint32_t>({ 0, 2, 1, 0 })
			&& world.lookupAgent(journeys[0].id).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(journeys[1].id).entity->getSector() == world.getSector(lower).get()
			&& world.lookupAgent(journeys[2].id).entity->getSector() == world.getSector(middle).get();
	}

	bool liftFailuresCancellationAndDisableDrainSafely()
	{
		// Repeated selector failures keep the landing open and eventually return the
		// passenger to the current stop without leaking lift ownership.
		{
			core::World world("Failed lift selector", 6, 4);
			auto lower = world.addCorridor(0, 0, 5);
			auto upper = world.addCorridor(2, 0, 5);
			core::World::CreateLiftOptions options;
			options.stopOffsets = { 0, 2 };
			auto created = world.addLift(1, 0, 2, options);
			world.finishBuild();
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(upper).get(), { 2.5f, 2.0f });
			auto id = world.createAgent("Failed selector passenger", lower, 0, 2.5f);
			auto agent = world.lookupAgent(id).entity;
			agent->setPath(world.getGraph()->calculatePath(agent, target), true);
			std::set<core::DeviceOperationId> failed;
			bool stayedOpen = true;
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 5; ++tick)
			{
				world.advanceTick();
				auto snapshot = world.getSimulationSnapshot();
				for (auto const& operation : snapshot.deviceOperations)
				{
					if (operation.command.type != core::DeviceCommandType::SelectLiftDestination
						|| failed.contains(operation.id)
						|| (operation.state != core::DeviceOperationState::Pending
							&& operation.state != core::DeviceOperationState::Running)) continue;
					world.lookupDeviceOperation(operation.id).entity->setState(core::DeviceOperationState::Failed);
					failed.insert(operation.id);
				}
				auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
					[&](auto const& resource) { return resource.id == created.traversalResource; });
				if (lift != snapshot.traversalResources.end() && lift->occupantCount > 0
					&& lift->liftStopPhase == core::LiftStopPhase::Closing) stayedOpen = false;
				if (failed.size() == 3 && agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(lower).get()) break;
			}
			auto final = world.getSimulationSnapshot();
			auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (failed.size() != 3 || !stayedOpen || lift == final.traversalResources.end()
				|| lift->occupantCount != 0 || lift->admissionReservationCount != 0
				|| !lift->admissionQueue.empty() || lift->liftPendingSafeExits != 0) return false;
		}

		// Cancellation while moving and subsequent disable both preserve occupancy
		// until alignment, reject fresh demand, and unload through a landing permit.
		{
			core::World world("Disabled moving lift", 6, 4);
			auto lower = world.addCorridor(0, 0, 5);
			auto upper = world.addCorridor(2, 0, 5);
			core::World::CreateLiftOptions options;
			options.stopOffsets = { 0, 2 };
			auto created = world.addLift(1, 0, 2, options);
			world.finishBuild();
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(upper).get(), { 2.5f, 2.0f });
			auto id = world.createAgent("Cancelled onboard passenger", lower, 0, 2.5f);
			auto agent = world.lookupAgent(id).entity;
			agent->setPath(world.getGraph()->calculatePath(agent, target), true);
			bool cancelledMoving = false;
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
			{
				world.advanceTick();
				auto snapshot = world.getSimulationSnapshot();
				auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
					[&](auto const& resource) { return resource.id == created.traversalResource; });
				if (!cancelledMoving && lift != snapshot.traversalResources.end() && lift->liftMoving)
				{
					agent->clearPath();
					cancelledMoving = world.setTraversalResourceEnabled(created.traversalResource, false);
				}
				if (cancelledMoving && agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(upper).get()) break;
			}
			world.advanceTick(); // publish the terminal unavailable state after unload commit
			auto final = world.getSimulationSnapshot();
			auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (!cancelledMoving || lift == final.traversalResources.end() || lift->enabled
				|| lift->liftDraining || lift->occupantCount != 0 || lift->liftPendingSafeExits != 0
				|| !lift->liftScheduledStops.empty() || !final.traversalRequests.empty()
				|| !final.traversalPermits.empty()) return false;
		}
		return true;
	}

	bool editorLiftAuthoringReconcilesOwnedLandings()
	{
		core::World world("Editor lift authoring", 10, 8);
		world.addCorridor(1, 0, 8);
		world.addCorridor(4, 0, 8);
		auto created = world.addLift(1, 0, 2, 2, 6);
		world.finishBuild();
		auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
		if (!lift || lift->getCellsWide() != 2 || lift->getLevelsHigh() != 6
			|| lift->getNumStops() != 2 || created.doors.size() != 2) return false;
		for (auto const& door : created.doors)
			if (!world.isLiftOwnedDoor(door.door.sector->getObject(door.door.index))) return false;

		world.pauseSimulation();
		world.addCorridor(3, 0, 8);
		world.finishBuild();
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			world.getSectorAtPosition(1, 2.0f, 0.0f));
		if (!lift || lift->getNumStops() != 2) return false; // Corridors do not create stops.
		uint32_t landingX = 0, landingWidth = 0;
		if (!world.getLiftLandingGeometry(1, 3, 3, landingX, landingWidth)
			|| landingX != 2 || landingWidth != 2) return false;
		auto added = world.addSectorDoor(0, 3, 3);
		if (!world.isLiftOwnedDoor(added.door.sector->getObject(added.door.index))) return false;
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			world.getSectorAtPosition(1, 2.0f, 0.0f));
		if (!lift || lift->getNumStops() != 3) return false;

		auto move = world.planResizeLift(lift->getIndex(), 5, 0, 2, 6);
		if (!move.valid || !move.move) return false;
		auto movedIndex = world.applyLiftEdit(move);
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(world.getSector(movedIndex));
		if (!lift || lift->getCellX() != 5 || lift->getLevelsHigh() != 6
			|| lift->getNumStops() != 3) return false;
		for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
		{
			auto level = (uint32_t)((int)lift->getStop(stop).sector->getCellY()
				+ lift->getStop(stop).sectorOffsetY);
			auto const& cell = static_cast<core::World const&>(world)
				.getLayer(0)->getCellDefinition(5, level);
			auto door = world.getSector(cell.sectorIndex)->getObject(cell.sectorObjectIndex);
			if (!world.isLiftOwnedDoor(door)) return false;
		}
		auto removeStop = world.planRemoveLiftStop(lift->getIndex(), 1);
		if (!removeStop.valid || !removeStop.requiresConfirmation()) return false;
		auto afterStopRemoval = world.applyLiftEdit(removeStop);
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(world.getSector(afterStopRemoval));
		if (!lift || lift->getNumStops() != 2) return false;
		auto remove = world.planRemoveLift(lift->getIndex());
		if (!remove.valid) return false;
		world.applyLiftEdit(remove);
		return !world.getSectorAtPosition(1, 5.0f, 0.0f);
	}
}

void registerLifts(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "singlePassengerCompletesTwoStopLiftJourney", [](smoke::Context const&) { smoke::require(singlePassengerCompletesTwoStopLiftJourney(), "singlePassengerCompletesTwoStopLiftJourney"); } });
	checks.push_back({ "liftDoorQueueRequestsBeforeOccupiedTail", [](smoke::Context const&) { smoke::require(liftDoorQueueRequestsBeforeOccupiedTail(), "liftDoorQueueRequestsBeforeOccupiedTail"); } });
	checks.push_back({ "liftCallOperatorDoesNotFightItsQueuePosition", [](smoke::Context const&) { smoke::require(liftCallOperatorDoesNotFightItsQueuePosition(), "liftCallOperatorDoesNotFightItsQueuePosition"); } });
	checks.push_back({ "waitingLiftPassengersFillArrivingCar", [](smoke::Context const&) { smoke::require(waitingLiftPassengersFillArrivingCar(), "waitingLiftPassengersFillArrivingCar"); } });
	checks.push_back({ "liftCapacityAndStopPhasesAreEnforced", [](smoke::Context const&) { smoke::require(liftCapacityAndStopPhasesAreEnforced(), "liftCapacityAndStopPhasesAreEnforced"); } });
	checks.push_back({ "multiStopLiftUsesDeterministicLookScheduling", [](smoke::Context const&) { smoke::require(multiStopLiftUsesDeterministicLookScheduling(), "multiStopLiftUsesDeterministicLookScheduling"); } });
	checks.push_back({ "liftFailuresCancellationAndDisableDrainSafely", [](smoke::Context const&) { smoke::require(liftFailuresCancellationAndDisableDrainSafely(), "liftFailuresCancellationAndDisableDrainSafely"); } });
	checks.push_back({ "editorLiftAuthoringReconcilesOwnedLandings", [](smoke::Context const&) { smoke::require(editorLiftAuthoringReconcilesOwnedLandings(), "editorLiftAuthoringReconcilesOwnedLandings"); } });
}
